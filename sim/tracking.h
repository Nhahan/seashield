#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/math.h"
#include "core/matrix.h"
#include "sim/radar.h"

// Target tracking (charter §5.5): a linear constant-velocity Kalman filter
// over converted radar measurements, plus the track-management shell —
// Mahalanobis gating, nearest-neighbour association, M-of-N initiation and
// miss-streak deletion. The operator never sees truth; this estimate is what
// the fire-control solver gets (§5.6), which is how sensor quality propagates
// into miss distance.
//
// Determinism: the tracker consumes NO randomness — every branch is a pure
// function of plots (whose randomness lives in the radar's seeded streams)
// and tick counters. Tracks are kept and iterated in ascending id order.
namespace seashield::sim {

// Constant-velocity Kalman filter, state x = [position, velocity] ∈ R⁶.
// Pure estimation: knows nothing about scans or track lifecycles, so it can
// be verified in isolation against the NumPy reference (kalman_ref.py).
class KalmanFilter {
 public:
  // F and the discrete white-noise-acceleration Q are fixed at construction:
  // the tick step never changes (charter §5.1 고정 타임스텝).
  KalmanFilter(double dt_s, double accel_noise_mps2);

  void predict();

  // Mahalanobis distance² of measurement z against the predicted state,
  // using S = HPHᵀ + R. nullopt if S is numerically singular — the caller
  // must discard the plot rather than gate on garbage.
  std::optional<double> gate_distance_sq(const math::Vec3& z, const math::Mat3& r) const;

  // Measurement update (Joseph form + symmetrization). False if S is
  // singular; the state is untouched in that case.
  bool update(const math::Vec3& z, const math::Mat3& r);

  math::Vec3 position() const { return {x_[0], x_[1], x_[2]}; }
  math::Vec3 velocity() const { return {x_[3], x_[4], x_[5]}; }
  const math::Vec6& state() const { return x_; }
  const math::Mat6& covariance() const { return p_; }
  void set_state(const math::Vec6& x, const math::Mat6& p) {
    x_ = x;
    p_ = p;
  }
  // Mean position variance — the wire-format track quality scalar.
  double position_variance() const { return (p_(0, 0) + p_(1, 1) + p_(2, 2)) / 3.0; }

 private:
  math::Mat6 f_;
  math::Mat6 q_;
  math::Vec6 x_;
  math::Mat6 p_;
};

enum class TrackStatus : std::uint8_t {
  kTentative = 0,
  kConfirmed = 1,
};

struct TrackerParams {
  double accel_noise_mps2 = 30.0;  // DWNA σ_a — sized to absorb ASM weaving.
  // Gate threshold for d² (χ², 3 dof, 0.999): generous so a maneuvering
  // target is coasted over rather than instantly shed.
  double gate_gamma = 16.27;
  int confirm_m = 3;  // Confirm when M of the last N scans associated.
  int confirm_n = 5;
  int drop_after_misses = 3;  // Consecutive missed SCANS (not ticks).
  double init_velocity_sigma_mps = 400.0;
};

struct Track {
  std::uint32_t id = 0;
  TrackStatus status = TrackStatus::kTentative;
  KalmanFilter filter;
  std::uint64_t last_update_tick = 0;
  std::uint32_t scan_history = 0;  // Bit i = associated in the i-th previous scan.
  int consecutive_missed_scans = 0;
  bool updated_this_scan = false;

  math::Vec3 position() const { return filter.position(); }
  math::Vec3 velocity() const { return filter.velocity(); }
  // A confirmed track that is currently missing scans is "coasting" — the
  // estimate extrapolates without measurements (wire state 2).
  bool coasting() const {
    return status == TrackStatus::kConfirmed && consecutive_missed_scans > 0;
  }
};

// Lifecycle transitions, drained by the World into its event log (and by the
// server into wire events). Append-only, tick-ordered.
struct TrackEvent {
  enum class Kind : std::uint8_t { kInitiated = 0, kConfirmed = 1, kDropped = 2 };
  std::uint64_t tick = 0;
  std::uint32_t track_id = 0;
  Kind kind = Kind::kInitiated;
};

class Tracker {
 public:
  explicit Tracker(const TrackerParams& params);

  // Every tick: advance all tracks to "now" (estimates stay current even
  // between scans, so fire control and snapshots never extrapolate manually).
  void predict();

  // Plot-to-track association for this tick's plots, in plot order: each plot
  // updates the gating track with the smallest d² (ties: lowest id), or
  // spawns a fresh tentative track if nothing gates.
  void update(std::span<const Plot> plots, std::uint64_t tick);

  // Scan-boundary bookkeeping: push hit/miss history, confirm M-of-N,
  // drop miss-streak tracks. Misses only make sense per SCAN — a tick where
  // the beam pointed elsewhere is not a detection failure.
  void on_scan_boundary(std::uint64_t tick);

  const std::vector<Track>& tracks() const { return tracks_; }
  const Track* find(std::uint32_t id) const;
  std::vector<TrackEvent> drain_events();
  std::uint32_t next_track_id() const { return next_track_id_; }
  const TrackerParams& params() const { return params_; }

 private:
  TrackerParams params_;
  std::vector<Track> tracks_;  // Ascending id (append-only growth + erase).
  std::vector<TrackEvent> events_;
  std::uint32_t next_track_id_ = 1;
};

}  // namespace seashield::sim
