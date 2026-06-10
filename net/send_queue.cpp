#include "net/send_queue.h"

namespace seashield::net {

bool SendQueue::push(std::vector<std::uint8_t> chunk) {
  if (chunk.empty()) {
    return true;
  }
  if (size_bytes_ + chunk.size() > max_bytes_) {
    overflowed_ = true;
    return false;
  }
  size_bytes_ += chunk.size();
  chunks_.push_back(std::move(chunk));
  return true;
}

SendQueue::FlushResult SendQueue::flush(const Writer& writer) {
  while (!chunks_.empty()) {
    const std::vector<std::uint8_t>& front = chunks_.front();
    const std::size_t remaining = front.size() - head_offset_;
    const long written = writer(front.data() + head_offset_, remaining);
    if (written < 0) {
      return FlushResult::kError;
    }
    if (written == 0) {
      return FlushResult::kWouldBlock;
    }

    const std::size_t consumed = static_cast<std::size_t>(written);
    size_bytes_ -= consumed;
    if (consumed < remaining) {
      head_offset_ += consumed;  // Partial write: resume here next flush.
      return FlushResult::kWouldBlock;
    }
    head_offset_ = 0;
    chunks_.pop_front();
  }
  return FlushResult::kDrained;
}

}  // namespace seashield::net
