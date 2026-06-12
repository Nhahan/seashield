#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "protocol/messages.h"

// Production client session engine (charter §7): the blocking TCP handshake →
// UDP bind → consume loop that the UE client runs on its FRunnable network
// thread. Everything UE-specific stays out — callbacks fire ON THE NETWORK
// THREAD and the caller marshals across (the UE wrapper pushes into TQueues).
// POSIX sockets, std-only otherwise; the same engine is integration-tested
// headlessly against the real server (tests/client_session_test.cpp).
namespace seashield::client {

struct ClientSessionConfig {
  std::string host = "127.0.0.1";
  std::uint16_t tcp_port = 7777;
  std::uint16_t udp_port = 0;  // 0 = use the port announced in the welcome.
  protocol::Role role = protocol::Role::kSolo;
  std::uint64_t token = 0;  // Nonzero = reconnect (charter §4.8).
  double udp_hello_timeout_s = 5.0;
  double udp_hello_interval_s = 0.2;
  double keepalive_interval_s = 0.5;
};

struct ClientSessionCallbacks {
  std::function<void(const protocol::ServerWelcome&)> on_welcome;
  std::function<void(protocol::RejectReason)> on_reject;
  std::function<void(const protocol::Snapshot&)> on_snapshot;            // Raw batch.
  std::function<void(const protocol::SnapshotDelta&)> on_snapshot_delta;  // Raw batch (v4).
  std::function<void(const protocol::EngagementEvent&)> on_event;
  std::function<void(const protocol::FireSolution&)> on_fire_solution;
  std::function<void(const std::string&)> on_error;
};

class ClientSession {
 public:
  ClientSession(ClientSessionConfig config, ClientSessionCallbacks callbacks)
      : config_(std::move(config)), callbacks_(std::move(callbacks)) {}

  // Blocking: returns when stop() is called, the server goes away, or an
  // error/reject ends the session early. False on the latter two.
  bool run();

  void stop() { stop_.store(true); }

  // Thread-safe: queued and sent over TCP from the session loop.
  void request_fire(const protocol::FireRequest& fire);

  // Thread-safe: the newest fully-assembled snapshot tick (v4 delta
  // baseline). Only the latest value matters, so this overwrites.
  void ack_snapshot(std::uint32_t tick) { pending_ack_.store(static_cast<std::int64_t>(tick)); }

 private:
  ClientSessionConfig config_;
  ClientSessionCallbacks callbacks_;
  std::atomic<bool> stop_{false};
  std::atomic<std::int64_t> pending_ack_{-1};
  std::mutex fire_mutex_;
  std::vector<protocol::FireRequest> pending_fire_;
};

}  // namespace seashield::client
