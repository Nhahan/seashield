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
  // Repeat fire: fire_count volleys spaced fire_interval_s apart starting at
  // fire_after_s — stress runs accumulate hundreds of live rockets this way.
  int fire_count = 1;
  double fire_interval_s = 1.0;
  // v4: assemble snapshots and ack them — the server then switches this
  // client to the delta stream. Off by default so legacy full-snapshot
  // statistics (and the tests pinned to them) keep their meaning; the
  // delta/bandwidth tests opt in.
  bool ack_snapshots = false;
  // With fire_at_track, the request designates the first CONFIRMED track seen
  // in snapshots (fire.track_id is filled in; fire.az/el become operator
  // offsets). The send is deferred until such a track exists.
  bool fire_at_track = false;
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
  // (kind, subject_id, tick) triple arriving twice means exactly-once broke.
  bool duplicate_event = false;

  // Track stream statistics (P4): what the console's PPI would have drawn.
  std::uint64_t track_records_seen = 0;
  std::uint8_t max_track_state_seen = 0;  // 0 tentative, 1 confirmed, 2 coasting.
  std::uint16_t last_track_count = 0;     // kTrack records in the latest batch.
  double last_track_sigma_m = 0.0;        // Dequantized quality of the last track.
  std::uint16_t designated_track_id = 0;  // Track fired upon (fire_at_track).

  // Fire-solution stream statistics (P5): the weapons console's PIP feed.
  std::uint64_t fire_solutions_seen = 0;
  std::uint64_t valid_fire_solutions_seen = 0;
  protocol::FireSolution last_fire_solution;

  // Raw downlink accounting (always on): every received UDP datagram.
  std::uint64_t udp_datagrams = 0;
  std::uint64_t udp_bytes = 0;
  // v4: events received via the bind-time TCP backlog (pre-dedup count).
  std::uint64_t backlog_events = 0;

  // v4 delta-path statistics (populated when config.ack_snapshots).
  std::uint64_t delta_batches = 0;
  std::uint64_t assembled_ticks = 0;        // Frames completed (full or delta).
  std::uint64_t delta_assembled_ticks = 0;  // Frames completed via delta.
  std::uint16_t last_assembled_entities = 0;

  // Welcome weather scalars (v3): the client-side visual drivers, echoed
  // here so integration tests can assert the round trip.
  double surface_wind_east_mps = 0.0;
  double surface_wind_north_mps = 0.0;
  double rain_intensity = 0.0;
  double gust_sigma_mps = 0.0;
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
