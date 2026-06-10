#include "net/acceptor.h"

#include <sys/socket.h>

#include <cerrno>
#include <utility>

#include "core/logger.h"
#include "net/socket_util.h"

namespace seashield::net {

Acceptor::Acceptor(EventLoop& loop, NewConnectionHandler handler)
    : loop_(loop), handler_(std::move(handler)) {}

Acceptor::~Acceptor() {
  if (listen_fd_.valid()) {
    loop_.remove(listen_fd_.get());
  }
}

bool Acceptor::listen(std::uint16_t port, int backlog) {
  listen_fd_ = create_tcp_listener(port, backlog);
  if (!listen_fd_.valid()) {
    return false;
  }
  return loop_.add(listen_fd_.get(), IoEvents::kRead, [this](unsigned) { on_readable(); });
}

std::uint16_t Acceptor::port() const {
  return listen_fd_.valid() ? local_port(listen_fd_.get()) : 0;
}

void Acceptor::on_readable() {
  for (;;) {
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
#if defined(__linux__)
    const int raw = ::accept4(listen_fd_.get(), reinterpret_cast<sockaddr*>(&peer), &peer_len,
                              SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    const int raw = ::accept(listen_fd_.get(), reinterpret_cast<sockaddr*>(&peer), &peer_len);
#endif
    if (raw < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;  // Drained.
      }
      if (errno == EMFILE || errno == ENFILE) {
        // Keeping the listener armed would busy-loop under level triggering:
        // the pending connection stays readable forever (design doc §8).
        SS_LOG_WARN("descriptor exhaustion (errno=%d); pausing accept", errno);
        pause();
        return;
      }
      if (errno == ECONNABORTED) {
        continue;  // Peer gave up between SYN and accept; not our problem.
      }
      SS_LOG_ERROR("accept failed: errno=%d", errno);
      return;
    }

    UniqueFd fd(raw);
#if !defined(__linux__)
    // No accept4 on macOS: apply both flags right after accept (two distinct
    // fcntl spaces: F_SETFL for O_NONBLOCK, F_SETFD for FD_CLOEXEC).
    if (!set_nonblocking_cloexec(fd.get())) {
      SS_LOG_WARN("set_nonblocking_cloexec failed for accepted fd; dropping");
      continue;
    }
#endif
    set_nosigpipe(fd.get());
    handler_(std::move(fd), peer);
  }
}

void Acceptor::pause() {
  if (!paused_ && listen_fd_.valid()) {
    paused_ = true;
    loop_.modify(listen_fd_.get(), 0);
  }
}

void Acceptor::resume() {
  if (paused_ && listen_fd_.valid()) {
    paused_ = false;
    loop_.modify(listen_fd_.get(), IoEvents::kRead);
  }
}

}  // namespace seashield::net
