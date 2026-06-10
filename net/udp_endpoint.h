#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "core/unique_fd.h"
#include "net/event_loop.h"

namespace seashield::net {

// Bound, non-blocking UDP socket on the event loop (design doc G2). Carries
// raw datagrams only; reliability/sequencing live in the P3 protocol layer.
class UdpEndpoint {
 public:
  using DatagramHandler = std::function<void(std::span<const std::uint8_t>, const sockaddr_in&)>;

  explicit UdpEndpoint(EventLoop& loop);
  ~UdpEndpoint();

  UdpEndpoint(const UdpEndpoint&) = delete;
  UdpEndpoint& operator=(const UdpEndpoint&) = delete;

  // Binds 0.0.0.0:port (0 = ephemeral) and registers with the loop.
  bool open(std::uint16_t port, DatagramHandler handler);

  std::uint16_t port() const;

  // Best-effort send: a full socket buffer (EAGAIN) drops the datagram and
  // bumps the drop counter — retrying stale state is worthless under UDP
  // semantics (design doc §8).
  bool send_to(std::span<const std::uint8_t> payload, const sockaddr_in& to);

  std::uint64_t dropped() const { return dropped_; }

 private:
  void on_readable();

  EventLoop& loop_;
  UniqueFd fd_;
  DatagramHandler handler_;
  std::uint64_t dropped_ = 0;
  std::vector<std::uint8_t> buf_;
};

}  // namespace seashield::net
