#pragma once

#include <netinet/in.h>

#include <cstdint>

#include "core/unique_fd.h"

namespace seashield::net {

// Makes fd non-blocking and close-on-exec. Returns false on fcntl failure.
bool set_nonblocking_cloexec(int fd);

// Process-wide SIGPIPE ignore; per-socket guards are applied additionally
// (SO_NOSIGPIPE on macOS, MSG_NOSIGNAL on Linux). Design doc §8.
void ignore_sigpipe();

// Suppresses SIGPIPE on this socket where supported (macOS SO_NOSIGPIPE).
void set_nosigpipe(int fd);

// send() flags that suppress SIGPIPE where supported (Linux MSG_NOSIGNAL).
int send_flags_nosignal();

// Creates a non-blocking TCP listener bound to 0.0.0.0:port (port 0 picks an
// ephemeral port). Returns an invalid UniqueFd on failure.
UniqueFd create_tcp_listener(std::uint16_t port, int backlog = 128);

// Creates a non-blocking UDP socket bound to 0.0.0.0:port.
UniqueFd create_udp_socket(std::uint16_t port);

// Returns the locally bound port of fd, or 0 on failure.
std::uint16_t local_port(int fd);

}  // namespace seashield::net
