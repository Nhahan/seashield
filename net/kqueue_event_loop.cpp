#if defined(__APPLE__) || defined(__FreeBSD__)

#include "net/kqueue_event_loop.h"

#include <sys/event.h>
#include <sys/time.h>

#include <cerrno>
#include <utility>

#include "core/logger.h"

namespace seashield::net {

namespace {
constexpr uintptr_t kWakeupIdent = 1;
constexpr int kMaxEvents = 64;
}  // namespace

KqueueEventLoop::KqueueEventLoop() : kq_(::kqueue()) {
  if (!kq_.valid()) {
    SS_LOG_ERROR("kqueue() failed: errno=%d", errno);
    return;
  }
  struct kevent ev;
  EV_SET(&ev, kWakeupIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
  if (::kevent(kq_.get(), &ev, 1, nullptr, 0, nullptr) < 0) {
    SS_LOG_ERROR("EVFILT_USER registration failed: errno=%d", errno);
    kq_.reset();
  }
}

bool KqueueEventLoop::apply_interest(int fd, unsigned prev, unsigned next,
                                     std::uint64_t generation) {
  // The generation rides in udata and comes back with each event, so dispatch
  // can detect events that belong to a previous registration of a reused fd
  // number (design doc §3.1).
  void* const udata = reinterpret_cast<void*>(static_cast<uintptr_t>(generation));
  struct kevent changes[2];
  int change_count = 0;
  const bool had_read = (prev & IoEvents::kRead) != 0;
  const bool want_read = (next & IoEvents::kRead) != 0;
  const bool had_write = (prev & IoEvents::kWrite) != 0;
  const bool want_write = (next & IoEvents::kWrite) != 0;

  if (want_read != had_read) {
    EV_SET(&changes[change_count++], fd, EVFILT_READ, want_read ? EV_ADD : EV_DELETE, 0, 0, udata);
  }
  if (want_write != had_write) {
    EV_SET(&changes[change_count++], fd, EVFILT_WRITE, want_write ? EV_ADD : EV_DELETE, 0, 0,
           udata);
  }
  if (change_count == 0) {
    return true;
  }
  if (::kevent(kq_.get(), changes, change_count, nullptr, 0, nullptr) < 0) {
    SS_LOG_ERROR("kevent change failed: fd=%d errno=%d", fd, errno);
    return false;
  }
  return true;
}

bool KqueueEventLoop::add(int fd, unsigned interest, IoCallback callback) {
  if (!valid() || fd < 0 || entries_.count(fd) != 0) {
    return false;
  }
  const std::uint64_t generation = next_generation_++;
  if (!apply_interest(fd, 0, interest, generation)) {
    return false;
  }
  entries_[fd] = Entry{interest, generation, std::move(callback)};
  return true;
}

bool KqueueEventLoop::modify(int fd, unsigned interest) {
  auto it = entries_.find(fd);
  if (it == entries_.end()) {
    return false;
  }
  if (!apply_interest(fd, it->second.interest, interest, it->second.generation)) {
    return false;
  }
  it->second.interest = interest;
  return true;
}

bool KqueueEventLoop::remove(int fd) {
  auto it = entries_.find(fd);
  if (it == entries_.end()) {
    return false;
  }
  apply_interest(fd, it->second.interest, 0, it->second.generation);
  entries_.erase(it);
  return true;
}

int KqueueEventLoop::run_once(int timeout_ms) {
  struct kevent events[kMaxEvents];
  struct timespec ts;
  struct timespec* ts_ptr = nullptr;
  if (timeout_ms >= 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
    ts_ptr = &ts;
  }

  const int ready = ::kevent(kq_.get(), nullptr, 0, events, kMaxEvents, ts_ptr);
  if (ready < 0) {
    if (errno == EINTR) {
      return 0;
    }
    SS_LOG_ERROR("kevent wait failed: errno=%d", errno);
    return -1;
  }

  int dispatched = 0;
  for (int i = 0; i < ready; ++i) {
    const struct kevent& ev = events[i];
    if (ev.filter == EVFILT_USER) {
      ++dispatched;  // wakeup() notification; EV_CLEAR auto-resets it.
      continue;
    }

    unsigned bits = 0;
    if (ev.filter == EVFILT_READ) {
      bits |= IoEvents::kRead;
    }
    if (ev.filter == EVFILT_WRITE) {
      bits |= IoEvents::kWrite;
    }
    if ((ev.flags & EV_EOF) != 0) {
      bits |= IoEvents::kHangup;
    }
    if ((ev.flags & EV_ERROR) != 0) {
      bits |= IoEvents::kError;
    }

    // Fresh lookup: an earlier callback in this batch may have removed fd.
    const int fd = static_cast<int>(ev.ident);
    auto it = entries_.find(fd);
    if (it == entries_.end()) {
      continue;
    }
    // Generation check: fd numbers are reused immediately after close, so an
    // event captured for a previous registration must not reach the entry
    // that now occupies the same fd number.
    const auto generation = static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(ev.udata));
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

void KqueueEventLoop::wakeup() {
  struct kevent ev;
  EV_SET(&ev, kWakeupIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
  ::kevent(kq_.get(), &ev, 1, nullptr, 0, nullptr);
}

}  // namespace seashield::net

#endif  // defined(__APPLE__) || defined(__FreeBSD__)
