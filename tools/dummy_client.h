#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "protocol/messages.h"

namespace seashield::tools {

// Headless protocol client (charter §4.1 DUMMY 클라이언트): speaks the full
// P3 protocol — TCP hello/welcome, UDP bind, snapshot consumption, reliable
// event acknowledgment — without any presentation layer. Used three ways:
// load-test driver (N instances), integration-test peer, and the reference
// implementation the UE5 client (P5) will mirror.
struct DummyClientConfig {
  std::string host = "127.0.0.1";
  std::uint16_t tcp_port = 0;
  // 0 = use the UDP port from ServerWelcome. Nonzero overrides it — that is
  // how traffic is routed through the chaos proxy (charter §10.3).
  std::uint16_t udp_port = 0;
  protocol::Role role = protocol::Role::kObserver;
  std::uint64_t token = 0;  // Nonzero = reconnect with a prior session token.
  double duration_s = 5.0;  // Run time after the UDP channel is up.
  // >= 0: send one FireRequest this long after the UDP bind completes
  // (requires weapons/solo role server-side).
  double fire_after_s = -1.0;
  protocol::FireRequest fire;
  double keepalive_interval_s = 0.1;  // 10Hz: carries acks for reliable events.
  double udp_hello_interval_s = 0.2;
  double udp_hello_timeout_s = 5.0;
};

struct DummyClientReport {
  bool connected = false;
  bool welcomed = false;
  bool rejected = false;
  protocol::RejectReason reject_reason = protocol::RejectReason::kVersionMismatch;
  bool udp_bound = false;
  bool disconnected_early = false;
  std::string error;

  std::uint64_t token = 0;
  protocol::Role role = protocol::Role::kObserver;
  std::string weather_summary;

  std::uint64_t snapshot_batches = 0;
  std::uint64_t snapshot_ticks = 0;  // Distinct ticks observed.
  std::uint32_t last_tick = 0;
  std::uint16_t last_total_entities = 0;
  protocol::EngagementPhase last_phase = protocol::EngagementPhase::kRunning;

  std::vector<protocol::EngagementEvent> events;
  // Application-level double check on top of the reliable layer's dedup: a
  // (kind, rocket_id, tick) triple arriving twice means exactly-once broke.
  bool duplicate_event = false;
};

class DummyClient {
 public:
  explicit DummyClient(DummyClientConfig config) : config_(std::move(config)) {}

  // Blocking: TCP handshake -> UDP bind -> consume until duration elapses,
  // the engagement ends and goes quiet, or stop() is called.
  DummyClientReport run();

  // Thread-safe early stop (reconnect tests drop the client mid-engagement).
  void stop() { stop_.store(true); }

 private:
  DummyClientConfig config_;
  std::atomic<bool> stop_{false};
};

}  // namespace seashield::tools
