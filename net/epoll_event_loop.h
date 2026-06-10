#pragma once

#if defined(__linux__)

#include <unordered_map>

#include "core/unique_fd.h"
#include "net/event_loop.h"

namespace seashield::net {

// epoll backend. epoll registers one bitmask per fd, so interest transitions
// map to a single EPOLL_CTL_MOD. Wakeup uses an eventfd registered for read
// (design doc §4.1).
class EpollEventLoop final : public EventLoop {
 public:
  EpollEventLoop();

  bool valid() const { return epoll_.valid() && wake_.valid(); }

  bool add(int fd, unsigned interest, IoCallback callback) override;
  bool modify(int fd, unsigned interest) override;
  bool remove(int fd) override;
  int run_once(int timeout_ms) override;
  void wakeup() override;

 private:
  struct Entry {
    unsigned interest = 0;
    IoCallback callback;
  };

  UniqueFd epoll_;
  UniqueFd wake_;
  std::unordered_map<int, Entry> entries_;
};

}  // namespace seashield::net

#endif  // defined(__linux__)
