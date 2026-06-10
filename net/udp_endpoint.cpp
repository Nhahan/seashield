#include "net/udp_endpoint.h"

#include <sys/socket.h>

#include <cerrno>
#include <utility>

#include "core/logger.h"
#include "net/socket_util.h"

namespace seashield::net {

namespace {
constexpr std::size_t kReceiveBufferSize = 64 * 1024;  // Max UDP datagram.
}

UdpEndpoint::UdpEndpoint(EventLoop& loop) : loop_(loop), buf_(kReceiveBufferSize) {}

UdpEndpoint::~UdpEndpoint() {
  if (fd_.valid()) {
    loop_.remove(fd_.get());
  }
}

bool UdpEndpoint::open(std::uint16_t port, DatagramHandler handler) {
  fd_ = create_udp_socket(port);
  if (!fd_.valid()) {
    return false;
  }
  set_nosigpipe(fd_.get());
  handler_ = std::move(handler);
  return loop_.add(fd_.get(), IoEvents::kRead, [this](unsigned) { on_readable(); });
}

std::uint16_t UdpEndpoint::port() const { return fd_.valid() ? local_port(fd_.get()) : 0; }

void UdpEndpoint::on_readable() {
  for (;;) {
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const ssize_t n = ::recvfrom(fd_.get(), buf_.data(), buf_.size(), 0,
                                 reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;  // Drained.
      }
      SS_LOG_WARN("recvfrom failed: errno=%d", errno);
      return;
    }
    // n == 0 is a valid empty datagram; deliver it like any other.
    handler_(std::span<const std::uint8_t>(buf_.data(), static_cast<std::size_t>(n)), from);
  }
}

bool UdpEndpoint::send_to(std::span<const std::uint8_t> payload, const sockaddr_in& to) {
  for (;;) {
    const ssize_t n = ::sendto(fd_.get(), payload.data(), payload.size(), send_flags_nosignal(),
                               reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    if (n >= 0) {
      return true;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      ++dropped_;
      return false;
    }
    SS_LOG_WARN("sendto failed: errno=%d", errno);
    return false;
  }
}

}  // namespace seashield::net
