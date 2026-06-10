#include "net/socket_util.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "core/logger.h"

namespace seashield::net {

bool set_nonblocking_cloexec(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return false;
  }
  const int fd_flags = ::fcntl(fd, F_GETFD, 0);
  return fd_flags >= 0 && ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) >= 0;
}

void ignore_sigpipe() { ::signal(SIGPIPE, SIG_IGN); }

void set_nosigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void)fd;
#endif
}

int send_flags_nosignal() {
#ifdef MSG_NOSIGNAL
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

namespace {

UniqueFd create_bound_socket(int type, std::uint16_t port) {
  UniqueFd fd(::socket(AF_INET, type, 0));
  if (!fd.valid()) {
    SS_LOG_ERROR("socket() failed: errno=%d", errno);
    return {};
  }
  const int one = 1;
  ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
    SS_LOG_ERROR("bind(port=%u) failed: errno=%d", port, errno);
    return {};
  }
  if (!set_nonblocking_cloexec(fd.get())) {
    SS_LOG_ERROR("set_nonblocking_cloexec failed: errno=%d", errno);
    return {};
  }
  return fd;
}

}  // namespace

UniqueFd create_tcp_listener(std::uint16_t port, int backlog) {
  UniqueFd fd = create_bound_socket(SOCK_STREAM, port);
  if (fd.valid() && ::listen(fd.get(), backlog) < 0) {
    SS_LOG_ERROR("listen() failed: errno=%d", errno);
    return {};
  }
  return fd;
}

UniqueFd create_udp_socket(std::uint16_t port) { return create_bound_socket(SOCK_DGRAM, port); }

std::uint16_t local_port(int fd) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    return 0;
  }
  return ntohs(addr.sin_port);
}

}  // namespace seashield::net
