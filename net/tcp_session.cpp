#include "net/tcp_session.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

#include "core/logger.h"
#include "net/socket_util.h"

namespace seashield::net {

TcpSession::TcpSession(EventLoop& loop, UniqueFd fd, std::uint64_t id, std::size_t send_cap)
    : loop_(loop),
      fd_(std::move(fd)),
      id_(id),
      send_queue_(send_cap),
      read_buf_(FrameParser::kHeaderSize + FrameParser::kMaxPayloadSize) {}

TcpSession::~TcpSession() {
  if (state_ != State::kDisconnected && fd_.valid()) {
    loop_.remove(fd_.get());
  }
}

bool TcpSession::start(FrameHandler on_frame, CloseHandler on_close) {
  on_frame_ = std::move(on_frame);
  on_close_ = std::move(on_close);
  if (!loop_.add(fd_.get(), IoEvents::kRead, [this](unsigned events) { on_events(events); })) {
    return false;
  }
  state_ = State::kActive;
  return true;
}

void TcpSession::on_events(unsigned events) {
  if ((events & IoEvents::kError) != 0) {
    int error = 0;
    socklen_t len = sizeof(error);
    ::getsockopt(fd_.get(), SOL_SOCKET, SO_ERROR, &error, &len);
    SS_LOG_WARN("session %llu socket error: %d", static_cast<unsigned long long>(id_), error);
    close("socket error");
    return;
  }
  if ((events & IoEvents::kWrite) != 0) {
    if (!flush_and_update_interest()) {
      return;
    }
  }
  if ((events & (IoEvents::kRead | IoEvents::kHangup)) != 0) {
    // Hangup is a hint; read()'s 0/error return is the authority (design §4.1).
    handle_readable();
  }
}

void TcpSession::handle_readable() {
  while (state_ == State::kActive) {
    const ssize_t n = ::read(fd_.get(), read_buf_.data(), read_buf_.size());
    if (n > 0) {
      const bool ok =
          parser_.feed(std::span<const std::uint8_t>(read_buf_.data(), static_cast<std::size_t>(n)),
                       [this](std::span<const std::uint8_t> frame) {
                         if (state_ == State::kActive) {
                           on_frame_(*this, frame);
                         }
                       });
      if (!ok) {
        close("protocol violation");
        return;
      }
      continue;
    }
    if (n == 0) {
      close("peer closed");
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;  // Drained.
    }
    close("read error");
    return;
  }
}

long TcpSession::write_some(const std::uint8_t* data, std::size_t size) {
  for (;;) {
    const ssize_t n = ::send(fd_.get(), data, size, send_flags_nosignal());
    if (n >= 0) {
      return static_cast<long>(n);
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }
    return -1;
  }
}

bool TcpSession::flush_and_update_interest() {
  const auto result = send_queue_.flush(
      [this](const std::uint8_t* data, std::size_t size) { return write_some(data, size); });
  if (result == SendQueue::FlushResult::kError) {
    close("write error");
    return false;
  }
  const bool want_write = !send_queue_.empty();
  if (want_write != write_interest_) {
    write_interest_ = want_write;
    loop_.modify(fd_.get(), IoEvents::kRead | (want_write ? IoEvents::kWrite : 0u));
  }
  return true;
}

bool TcpSession::send(std::span<const std::uint8_t> payload) {
  if (state_ != State::kActive || payload.empty() ||
      payload.size() > FrameParser::kMaxPayloadSize) {
    return false;
  }
  std::vector<std::uint8_t> frame;
  frame.reserve(FrameParser::kHeaderSize + payload.size());
  FrameParser::encode(frame, payload);
  if (!send_queue_.push(std::move(frame))) {
    // Isolation policy: a client that cannot drain its queue is evicted
    // instead of stalling the server or other clients (design doc §6.3).
    close("send queue overflow (slow client)");
    return false;
  }
  return flush_and_update_interest();
}

void TcpSession::close(const char* reason) {
  if (state_ == State::kDisconnected) {
    return;
  }
  state_ = State::kDisconnected;
  if (fd_.valid()) {
    loop_.remove(fd_.get());
    fd_.reset();
  }
  if (on_close_) {
    on_close_(*this, reason);
  }
}

}  // namespace seashield::net
