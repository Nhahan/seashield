#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace seashield::net {

// Stateful length-prefix framing for the TCP byte stream (design doc §5).
// Wire format: [length: uint16 LE][payload: length bytes], length in [1, 16384].
//
// Pure logic: knows nothing about sockets, so it is unit-testable with byte
// arrays alone.
class FrameParser {
 public:
  static constexpr std::size_t kHeaderSize = 2;
  static constexpr std::size_t kMaxPayloadSize = 16 * 1024;

  using FrameHandler = std::function<void(std::span<const std::uint8_t>)>;

  // Accumulates incoming bytes and invokes handler once per completed frame,
  // in order. Returns false on a protocol violation (zero-length or oversized
  // frame); the caller is expected to drop the connection.
  bool feed(std::span<const std::uint8_t> data, const FrameHandler& handler);

  std::size_t buffered_bytes() const { return buffer_.size(); }

  // Appends [header][payload] to out.
  static void encode(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> payload);

 private:
  std::vector<std::uint8_t> buffer_;
};

}  // namespace seashield::net
