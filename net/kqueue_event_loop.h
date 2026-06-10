#pragma once

#if defined(__APPLE__) || defined(__FreeBSD__)

#include <cstdint>
#include <unordered_map>

#include "core/unique_fd.h"
#include "net/event_loop.h"

namespace seashield::net {

// kqueue backend. kqueue registers (fd, filter) pairs, so READ and WRITE
// interest are tracked as separate filters; interest transitions are applied
// as diffs against the previously registered set (design doc §4.2).
class KqueueEventLoop final : public EventLoop {
 public:
  KqueueEventLoop();

  bool valid() const { return kq_.valid(); }

  bool add(int fd, unsigned interest, IoCallback callback) override;
  bool modify(int fd, unsigned interest) override;
  bool remove(int fd) override;
  int run_once(int timeout_ms) override;
  void wakeup() override;

 private:
  struct Entry {
    unsigned interest = 0;
    std::uint64_t generation = 0;
    IoCallback callback;
  };

  bool apply_interest(int fd, unsigned prev, unsigned next, std::uint64_t generation);

  UniqueFd kq_;
  std::uint64_t next_generation_ = 1;
  std::unordered_map<int, Entry> entries_;
};

}  // namespace seashield::net

#endif  // defined(__APPLE__) || defined(__FreeBSD__)
