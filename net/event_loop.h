#pragma once

#include <functional>
#include <memory>

namespace seashield::net {

// Bitmask of readiness events delivered to an IoCallback.
struct IoEvents {
  static constexpr unsigned kRead = 1u << 0;
  static constexpr unsigned kWrite = 1u << 1;
  static constexpr unsigned kError = 1u << 2;
  static constexpr unsigned kHangup = 1u << 3;
};

using IoCallback = std::function<void(unsigned events)>;

// Readiness-based (Reactor) event loop over kqueue/epoll. Contract details:
// docs/architecture/network-design.md §3-§4.
//
// Thread affinity: every method except wakeup() must be called from the loop
// thread (the thread that calls run_once()).
class EventLoop {
 public:
  // Returns the platform backend (kqueue on macOS/BSD, epoll on Linux), or
  // nullptr if the kernel facility could not be initialized.
  static std::unique_ptr<EventLoop> create();

  virtual ~EventLoop() = default;

  // Registers fd with an interest set (IoEvents bits). One registration per fd.
  virtual bool add(int fd, unsigned interest, IoCallback callback) = 0;
  // Replaces the interest set of a registered fd.
  virtual bool modify(int fd, unsigned interest) = 0;
  // Unregisters fd. Must be called before closing the descriptor.
  virtual bool remove(int fd) = 0;

  // Waits up to timeout_ms (-1 = no timeout) and dispatches ready events.
  // Returns the number of dispatched events, 0 on timeout, -1 on fatal error.
  virtual int run_once(int timeout_ms) = 0;

  // Thread-safe: unblocks a concurrent run_once(). Consecutive calls may
  // coalesce into a single wakeup.
  virtual void wakeup() = 0;
};

}  // namespace seashield::net
