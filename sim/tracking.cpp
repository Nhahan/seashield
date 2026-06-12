#include "sim/tracking.h"

#include <bit>

#include "sim/constants.h"

namespace seashield::sim {
namespace {

constexpr math::Mat<3, 6> measurement_matrix() {
  math::Mat<3, 6> h;
  h(0, 0) = 1.0;
  h(1, 1) = 1.0;
  h(2, 2) = 1.0;
  return h;
}

}  // namespace

// --- KalmanFilter ---------------------------------------------------------------

KalmanFilter::KalmanFilter(double dt_s, double accel_noise_mps2) {
  f_ = math::Mat6::identity();
  for (int axis = 0; axis < 3; ++axis) {
    f_(axis, axis + 3) = dt_s;
  }
  // Discrete white-noise acceleration: per-axis 2x2 block
  //   σ_a² · [[dt⁴/4, dt³/2], [dt³/2, dt²]]
  // over the (position_axis, velocity_axis) pair.
  const double sigma_sq = accel_noise_mps2 * accel_noise_mps2;
  const double dt2 = dt_s * dt_s;
  for (int axis = 0; axis < 3; ++axis) {
    q_(axis, axis) = sigma_sq * dt2 * dt2 / 4.0;
    q_(axis, axis + 3) = sigma_sq * dt2 * dt_s / 2.0;
    q_(axis + 3, axis) = q_(axis, axis + 3);
    q_(axis + 3, axis + 3) = sigma_sq * dt2;
  }
}

void KalmanFilter::predict() {
  x_ = f_ * x_;
  p_ = f_ * p_ * f_.transposed() + q_;
}

std::optional<double> KalmanFilter::gate_distance_sq(const math::Vec3& z,
                                                     const math::Mat3& r) const {
  static constexpr math::Mat<3, 6> kH = measurement_matrix();
  const math::Mat3 s = kH * p_ * kH.transposed() + r;
  const auto s_inv = math::inverse_spd(s);
  if (!s_inv) {
    return std::nullopt;
  }
  math::Mat<3, 1> nu;
  nu[0] = z.x - x_[0];
  nu[1] = z.y - x_[1];
  nu[2] = z.z - x_[2];
  const math::Mat<1, 1> d_sq = nu.transposed() * *s_inv * nu;
  return d_sq(0, 0);
}

bool KalmanFilter::update(const math::Vec3& z, const math::Mat3& r) {
  static constexpr math::Mat<3, 6> kH = measurement_matrix();
  const math::Mat3 s = kH * p_ * kH.transposed() + r;
  const auto s_inv = math::inverse_spd(s);
  if (!s_inv) {
    return false;
  }
  const math::Mat<6, 3> k = p_ * kH.transposed() * *s_inv;
  math::Mat<3, 1> nu;
  nu[0] = z.x - x_[0];
  nu[1] = z.y - x_[1];
  nu[2] = z.z - x_[2];
  x_ = x_ + k * nu;
  // Joseph form keeps P positive semi-definite under roundoff; the explicit
  // re-symmetrization mops up what is left. P's full 36 entries are hashed by
  // the world, so any asymmetry drift would surface as a determinism break.
  const math::Mat6 i_kh = math::Mat6::identity() - k * kH;
  p_ = i_kh * p_ * i_kh.transposed() + k * r * k.transposed();
  p_ = (p_ + p_.transposed()) * 0.5;
  return true;
}

// --- Tracker --------------------------------------------------------------------

Tracker::Tracker(const TrackerParams& params) : params_(params) {}

const Track* Tracker::find(std::uint32_t id) const {
  for (const Track& track : tracks_) {
    if (track.id == id) {
      return &track;
    }
  }
  return nullptr;
}

std::vector<TrackEvent> Tracker::drain_events() {
  std::vector<TrackEvent> drained = std::move(events_);
  events_.clear();
  return drained;
}

void Tracker::predict() {
  for (Track& track : tracks_) {
    track.filter.predict();
  }
}

void Tracker::update(std::span<const Plot> plots, std::uint64_t tick) {
  for (const Plot& plot : plots) {
    // Nearest neighbour over gating tracks; id-ascending iteration plus
    // strict '<' makes ties resolve to the lowest id — documented determinism.
    Track* best = nullptr;
    double best_d_sq = params_.gate_gamma;
    for (Track& track : tracks_) {
      const auto d_sq = track.filter.gate_distance_sq(plot.position, plot.covariance);
      if (d_sq.has_value() && *d_sq < best_d_sq) {
        best = &track;
        best_d_sq = *d_sq;
      }
    }
    if (best != nullptr) {
      if (best->filter.update(plot.position, plot.covariance)) {
        best->last_update_tick = tick;
        best->updated_this_scan = true;
      }
      continue;
    }

    // No track gates: single-plot initiation. Velocity is unknown, so it
    // starts at zero with a wide prior — M-of-N keeps such infants honest
    // (two-point initiation is the documented alternative; this is simpler
    // and converges within a couple of scans).
    Track track{
        .id = next_track_id_++,
        .status = TrackStatus::kTentative,
        .filter = KalmanFilter(kTickDt, params_.accel_noise_mps2),
    };
    math::Vec6 x0;
    x0[0] = plot.position.x;
    x0[1] = plot.position.y;
    x0[2] = plot.position.z;
    math::Mat6 p0;
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        p0(r, c) = plot.covariance(r, c);
      }
    }
    const double v_sigma_sq = params_.init_velocity_sigma_mps * params_.init_velocity_sigma_mps;
    for (int axis = 3; axis < 6; ++axis) {
      p0(axis, axis) = v_sigma_sq;
    }
    track.filter.set_state(x0, p0);
    track.last_update_tick = tick;
    track.updated_this_scan = true;
    tracks_.push_back(track);
    events_.push_back({tick, track.id, TrackEvent::Kind::kInitiated});
  }
}

void Tracker::on_scan_boundary(std::uint64_t tick) {
  // confirm_n is validated to [1, 32]; the branch keeps the shift in-width
  // at the 32 boundary (1u << 32 is undefined behaviour).
  const std::uint32_t history_mask =
      params_.confirm_n >= 32
          ? 0xFFFFFFFFu
          : ((1u << static_cast<unsigned>(params_.confirm_n)) - 1u);
  for (auto it = tracks_.begin(); it != tracks_.end();) {
    Track& track = *it;
    const bool hit = track.updated_this_scan;
    track.updated_this_scan = false;
    track.scan_history = ((track.scan_history << 1) | (hit ? 1u : 0u)) & history_mask;
    track.consecutive_missed_scans = hit ? 0 : track.consecutive_missed_scans + 1;

    if (track.status == TrackStatus::kTentative &&
        std::popcount(track.scan_history) >= params_.confirm_m) {
      track.status = TrackStatus::kConfirmed;
      events_.push_back({tick, track.id, TrackEvent::Kind::kConfirmed});
    }
    if (track.consecutive_missed_scans >= params_.drop_after_misses) {
      events_.push_back({tick, track.id, TrackEvent::Kind::kDropped});
      it = tracks_.erase(it);
      continue;
    }
    ++it;
  }
}

}  // namespace seashield::sim
