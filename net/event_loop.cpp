#include "net/event_loop.h"

#if defined(__APPLE__) || defined(__FreeBSD__)
#include "net/kqueue_event_loop.h"
#elif defined(__linux__)
#include "net/epoll_event_loop.h"
#else
#error "Unsupported platform: SeaShield requires kqueue or epoll"
#endif

namespace seashield::net {

std::unique_ptr<EventLoop> EventLoop::create() {
#if defined(__APPLE__) || defined(__FreeBSD__)
  auto loop = std::make_unique<KqueueEventLoop>();
#elif defined(__linux__)
  auto loop = std::make_unique<EpollEventLoop>();
#endif
  if (!loop->valid()) {
    return nullptr;
  }
  return loop;
}

}  // namespace seashield::net
