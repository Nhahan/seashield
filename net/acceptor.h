#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <functional>

#include "core/unique_fd.h"
#include "net/event_loop.h"

namespace seashield::net {

// Non-blocking TCP listener. Drains accept() until EAGAIN per readiness event
// and hands each connection (already non-blocking + SIGPIPE-guarded) to the
// handler.
//
// On fd exhaustion (EMFILE/ENFILE) the listener pauses its read interest to
// avoid a level-triggered busy-loop; the owner calls resume() once a session
// closes and descriptors are available again (design doc §8).
class Acceptor {
 public:
  using NewConnectionHandler = std::function<void(UniqueFd fd, const sockaddr_in& peer)>;

  Acceptor(EventLoop& loop, NewConnectionHandler handler);
  ~Acceptor();

  Acceptor(const Acceptor&) = delete;
  Acceptor& operator=(const Acceptor&) = delete;

  // Binds 0.0.0.0:port (0 = ephemeral) and registers with the loop.
  bool listen(std::uint16_t port, int backlog = 128);

  // Actual bound port (useful with ephemeral ports in tests).
  std::uint16_t port() const;

  bool paused() const { return paused_; }
  void resume();

 private:
  void on_readable();
  void pause();

  EventLoop& loop_;
  NewConnectionHandler handler_;
  UniqueFd listen_fd_;
  bool paused_ = false;
};

}  // namespace seashield::net
