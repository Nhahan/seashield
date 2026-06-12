#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <vector>

#include "protocol/messages.h"

// Client-side snapshot pipeline (charter §7 "수신 스냅샷 → 보간 버퍼 → 액터
// 트랜스폼"): reassemble batched snapshots, then sample entity states at a
// delayed render time. std-only so the logic is testable without UE; the UE
// module feeds decoded protocol::Snapshot batches in and reads samples out.
namespace seashield::client {

struct CompletedSnapshot {
  std::uint32_t tick = 0;
  protocol::EngagementPhase phase = protocol::EngagementPhase::kRunning;
  std::vector<protocol::EntityRecord> entities;
};

// Reassembles snapshot batches (Snapshot.first_index / total_entities) into
// complete per-tick entity lists. Unreliable transport means batches arrive
// out of order, duplicated, or never; ticks older than the newest completed
// one are dropped (a late-completing stale frame must not rewind the view).
//
// v4 delta path: push_delta() assembles SnapshotDelta batches the same way,
// then reconstructs the frame against the BASELINE snapshot this assembler
// completed earlier (the one the client acked). A delta whose baseline is
// gone is dropped — the server falls back to full snapshots on its own when
// acks stop advancing, so the stream self-heals.
class SnapshotAssembler {
 public:
  explicit SnapshotAssembler(std::uint16_t tick_rate_hz = 60) : tick_rate_hz_(tick_rate_hz) {}

  std::optional<CompletedSnapshot> push(const protocol::Snapshot& batch);
  std::optional<CompletedSnapshot> push_delta(const protocol::SnapshotDelta& batch);

  // What the client should ack (v4): the newest fully-assembled tick.
  std::optional<std::uint32_t> latest_completed_tick() const {
    return has_completed_ ? std::optional<std::uint32_t>(newest_completed_) : std::nullopt;
  }

 private:
  struct Partial {
    std::uint16_t total = 0;
    std::size_t received = 0;
    protocol::EngagementPhase phase = protocol::EngagementPhase::kRunning;
    std::vector<protocol::EntityRecord> entities;
    std::vector<bool> filled;
  };
  struct DeltaPartial {
    std::uint16_t total = 0;
    std::size_t received = 0;
    std::uint32_t base_tick = 0;
    protocol::EngagementPhase phase = protocol::EngagementPhase::kRunning;
    std::vector<protocol::DeltaEntity> entities;
    std::vector<bool> filled;
  };
  static constexpr std::size_t kMaxPendingTicks = 64;
  static constexpr std::size_t kMaxBaselines = 64;

  std::optional<CompletedSnapshot> complete(CompletedSnapshot done);
  bool stale(std::uint32_t tick) const { return has_completed_ && tick <= newest_completed_; }

  std::uint16_t tick_rate_hz_ = 60;
  std::map<std::uint32_t, Partial> partials_;
  std::map<std::uint32_t, DeltaPartial> delta_partials_;
  // Completed frames kept as delta baselines, keyed by tick (bounded).
  std::map<std::uint32_t, CompletedSnapshot> baselines_;
  std::uint32_t newest_completed_ = 0;
  bool has_completed_ = false;
};

struct SampledEntity {
  std::uint16_t id = 0;
  protocol::EntityKind kind = protocol::EntityKind::kTarget;
  std::uint8_t state = 0;
  std::uint8_t flags = 0;
  double pos_x = 0.0, pos_y = 0.0, pos_z = 0.0;  // ENU meters (coords.h maps).
  double vel_x = 0.0, vel_y = 0.0, vel_z = 0.0;
  bool extrapolated = false;
};

// Bounded history of completed snapshots sampled at a fractional tick time.
// Positions lerp between the bracketing snapshots; past the newest snapshot
// they extrapolate along the velocity, capped (a stalled feed must freeze,
// not fly away). kTrack entities never interpolate — smoothing a sensor
// estimate would fake a continuity the radar does not have (§5.5).
class InterpolationBuffer {
 public:
  explicit InterpolationBuffer(double tick_rate_hz = 60.0, double max_extrapolation_s = 0.25)
      : tick_rate_hz_(tick_rate_hz), max_extrapolation_ticks_(max_extrapolation_s * tick_rate_hz) {}

  void push(CompletedSnapshot snapshot);

  // Render-time sample. Empty before the first snapshot arrives.
  std::vector<SampledEntity> sample(double tick_time) const;

  // The delayed render clock: newest tick minus the interpolation delay
  // (~100 ms = 6 ticks for the 30 Hz snapshot stream). nullopt until data.
  std::optional<double> render_tick(double delay_ticks) const;

  std::size_t size() const { return history_.size(); }

 private:
  static constexpr std::size_t kMaxHistory = 32;

  double tick_rate_hz_;
  double max_extrapolation_ticks_;
  std::deque<CompletedSnapshot> history_;  // Ascending tick order.
};

}  // namespace seashield::client
