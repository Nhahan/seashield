#if defined(__linux__)

#include "net/epoll_event_loop.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <utility>

#include "core/logger.h"

namespace seashield::net {

namespace {

constexpr int kMaxEvents = 64;

uint32_t to_epoll_mask(unsigned interest) {
  uint32_t mask = EPOLLRDHUP;  // Always observe peer half-close.
  if ((interest & IoEvents::kRead) != 0) {
    mask |= EPOLLIN;
  }
  if ((interest & IoEvents::kWrite) != 0) {
    mask |= EPOLLOUT;
  }
  return mask;
}

// The generation rides in data.u64 (high 32 bits) next to the fd (low 32
// bits), so dispatch can detect events that belong to a previous registration
// of a reused fd number (design doc §3.1).
uint64_t pack_token(int fd, uint32_t generation) {
  return (static_cast<uint64_t>(generation) << 32) | static_cast<uint32_t>(fd);
}

}  // namespace

EpollEventLoop::EpollEventLoop()
    : epoll_(::epoll_create1(EPOLL_CLOEXEC)), wake_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
  if (!epoll_.valid() || !wake_.valid()) {
    SS_LOG_ERROR("epoll/eventfd init failed: errno=%d", errno);
    epoll_.reset();
    wake_.reset();
    return;
  }
  struct epoll_event ev {};
  ev.events = EPOLLIN;
  ev.data.u64 = pack_token(wake_.get(), 0);
  if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, wake_.get(), &ev) < 0) {
    SS_LOG_ERROR("registering wakeup eventfd failed: errno=%d", errno);
    epoll_.reset();
    wake_.reset();
  }
}

bool EpollEventLoop::apply(int op, int fd, unsigned interest, std::uint32_t generation) {
  struct epoll_event ev {};
  ev.events = to_epoll_mask(interest);
  ev.data.u64 = pack_token(fd, generation);
  if (::epoll_ctl(epoll_.get(), op, fd, &ev) < 0) {
    SS_LOG_ERROR("epoll_ctl(op=%d) failed: fd=%d errno=%d", op, fd, errno);
    return false;
  }
  return true;
}

bool EpollEventLoop::add(int fd, unsigned interest, IoCallback callback) {
  if (!valid() || fd < 0 || entries_.count(fd) != 0) {
    return false;
  }
  const std::uint32_t generation = next_generation_++;
  if (!apply(EPOLL_CTL_ADD, fd, interest, generation)) {
    return false;
  }
  entries_[fd] = Entry{interest, generation, std::move(callback)};
  return true;
}

bool EpollEventLoop::modify(int fd, unsigned interest) {
  auto it = entries_.find(fd);
  if (it == entries_.end()) {
    return false;
  }
  if (!apply(EPOLL_CTL_MOD, fd, interest, it->second.generation)) {
    return false;
  }
  it->second.interest = interest;
  return true;
}

bool EpollEventLoop::remove(int fd) {
  auto it = entries_.find(fd);
  if (it == entries_.end()) {
    return false;
  }
  struct epoll_event ev {};  // Dummy for pre-2.6.9 kernel compatibility.
  ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, fd, &ev);
  entries_.erase(it);
  return true;
}

int EpollEventLoop::run_once(int timeout_ms) {
  struct epoll_event events[kMaxEvents];
  const int ready = ::epoll_wait(epoll_.get(), events, kMaxEvents, timeout_ms);
  if (ready < 0) {
    if (errno == EINTR) {
      return 0;
    }
    SS_LOG_ERROR("epoll_wait failed: errno=%d", errno);
    return -1;
  }

  int dispatched = 0;
  for (int i = 0; i < ready; ++i) {
    const struct epoll_event& ev = events[i];
    const int fd = static_cast<int>(ev.data.u64 & 0xFFFFFFFFu);
    const auto generation = static_cast<std::uint32_t>(ev.data.u64 >> 32);
    if (fd == wake_.get()) {
      uint64_t value = 0;
      [[maybe_unused]] ssize_t n = ::read(wake_.get(), &value, sizeof(value));
      ++dispatched;
      continue;
    }

    unsigned bits = 0;
    if ((ev.events & EPOLLIN) != 0) {
      bits |= IoEvents::kRead;
    }
    if ((ev.events & EPOLLOUT) != 0) {
      bits |= IoEvents::kWrite;
    }
    if ((ev.events & EPOLLERR) != 0) {
      bits |= IoEvents::kError;
    }
    if ((ev.events & (EPOLLHUP | EPOLLRDHUP)) != 0) {
      bits |= IoEvents::kHangup;
    }

    // Fresh lookup: an earlier callback in this batch may have removed fd.
    auto it = entries_.find(fd);
    if (it == entries_.end()) {
      continue;
    }
    // Generation check: fd numbers are reused immediately after close, so an
    // event captured for a previous registration must not reach the entry
    // that now occupies the same fd number.
    if (generation != it->second.generation) {
      continue;
    }
    // Copy so the std::function survives self-removal from within the call.
    IoCallback callback = it->second.callback;
    ++dispatched;
    callback(bits);
  }
  return dispatched;
}

void EpollEventLoop::wakeup() {
  const uint64_t one = 1;
  // EAGAIN means the counter is saturated, i.e. a wakeup is already pending.
  [[maybe_unused]] ssize_t n = ::write(wake_.get(), &one, sizeof(one));
}

}  // namespace seashield::net

#endif  // defined(__linux__)
