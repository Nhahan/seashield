#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "core/unique_fd.h"
#include "net/event_loop.h"
#include "net/frame_parser.h"
#include "net/send_queue.h"

namespace seashield::net {

// One accepted TCP connection: length-prefix framing in, queued frames out,
// slow-client eviction via the send-queue cap (design doc §5-§6).
//
// Lifecycle: CONNECTING (constructed) -> ACTIVE (start()) -> DISCONNECTED
// (close()); P1 subset of the charter state machine, HANDSHAKE reserved for
// P3. close() is idempotent. The CloseHandler must only mark the session for
// deferred deletion — deleting it inside the callback stack is use-after-free
// (design doc §6.4).
class TcpSession {
 public:
  using FrameHandler = std::function<void(TcpSession&, std::span<const std::uint8_t>)>;
  using CloseHandler = std::function<void(TcpSession&, const char* reason)>;

  TcpSession(EventLoop& loop, UniqueFd fd, std::uint64_t id, std::size_t send_cap);
  ~TcpSession();

  TcpSession(const TcpSession&) = delete;
  TcpSession& operator=(const TcpSession&) = delete;

  // Registers with the loop; the session becomes ACTIVE.
  bool start(FrameHandler on_frame, CloseHandler on_close);

  // Frames and queues payload (1..16384 bytes). Returns false if the session
  // is not active, the payload size is invalid, or the queue cap was exceeded
  // — in the cap case the session closes itself (slow-client eviction).
  bool send(std::span<const std::uint8_t> payload);

  void close(const char* reason);

  std::uint64_t id() const { return id_; }
  int fd() const { return fd_.get(); }
  bool closed() const { return state_ == State::kDisconnected; }
  std::size_t send_backlog_bytes() const { return send_queue_.size_bytes(); }

 private:
  enum class State { kConnecting, kActive, kDisconnected };

  void on_events(unsigned events);
  void handle_readable();
  // Flushes the send queue and keeps WRITE interest registered only while
  // bytes remain (design doc §6.2). Returns false if the session closed.
  bool flush_and_update_interest();
  long write_some(const std::uint8_t* data, std::size_t size);

  EventLoop& loop_;
  UniqueFd fd_;
  std::uint64_t id_;
  State state_ = State::kConnecting;
  FrameParser parser_;
  SendQueue send_queue_;
  FrameHandler on_frame_;
  CloseHandler on_close_;
  bool write_interest_ = false;
  std::vector<std::uint8_t> read_buf_;
};

}  // namespace seashield::net
