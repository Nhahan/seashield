#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace seashield::net {

// Per-session outbound byte queue with a hard cap (design doc §6).
//
// The cap is the slow-client isolation mechanism: a client that cannot drain
// its queue is disconnected instead of stalling the server or other clients.
//
// Pure logic: the actual write syscall is injected, so partial writes and
// would-block paths are deterministically unit-testable.
class SendQueue {
 public:
  // writer contract: returns >0 bytes written, 0 for would-block, -1 for a
  // fatal error.
  using Writer = std::function<long(const std::uint8_t*, std::size_t)>;

  enum class FlushResult { kDrained, kWouldBlock, kError };

  explicit SendQueue(std::size_t max_bytes) : max_bytes_(max_bytes) {}

  // Returns false (and sets overflowed) if the cap would be exceeded.
  bool push(std::vector<std::uint8_t> chunk);

  // Writes queued chunks until drained, would-block, or error. Resumes
  // partially written chunks at the recorded offset.
  FlushResult flush(const Writer& writer);

  bool empty() const { return chunks_.empty(); }
  std::size_t size_bytes() const { return size_bytes_; }
  bool overflowed() const { return overflowed_; }

 private:
  std::size_t max_bytes_;
  std::size_t size_bytes_ = 0;
  std::size_t head_offset_ = 0;  // Consumed bytes of the front chunk.
  bool overflowed_ = false;
  std::deque<std::vector<std::uint8_t>> chunks_;
};

}  // namespace seashield::net
