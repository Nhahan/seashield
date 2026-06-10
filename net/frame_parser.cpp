#include "net/frame_parser.h"

namespace seashield::net {

bool FrameParser::feed(std::span<const std::uint8_t> data, const FrameHandler& handler) {
  buffer_.insert(buffer_.end(), data.begin(), data.end());

  std::size_t offset = 0;
  while (buffer_.size() - offset >= kHeaderSize) {
    const std::size_t length =
        static_cast<std::size_t>(buffer_[offset]) | (static_cast<std::size_t>(buffer_[offset + 1]) << 8);
    if (length == 0 || length > kMaxPayloadSize) {
      buffer_.clear();
      return false;
    }
    if (buffer_.size() - offset < kHeaderSize + length) {
      break;  // Frame incomplete; wait for more bytes.
    }
    handler(std::span<const std::uint8_t>(buffer_.data() + offset + kHeaderSize, length));
    offset += kHeaderSize + length;
  }

  // Single compaction per feed keeps the cost amortized O(1) per frame.
  buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
  return true;
}

void FrameParser::encode(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> payload) {
  const std::size_t length = payload.size();
  out.push_back(static_cast<std::uint8_t>(length & 0xFF));
  out.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFF));
  out.insert(out.end(), payload.begin(), payload.end());
}

}  // namespace seashield::net
