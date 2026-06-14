#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/matrix.h"
#include "core/math.h"
#include "core/pcg32.h"

// Rotating-scan radar (charter §5.4): the beam sweeps a full circle every
// scan period, and a target gets exactly one detection opportunity per scan —
// when the beam passes its azimuth. Detection is a Pd(R) roll on a sigmoid
// over SNR (∝ 1/R⁴ → linear in dB), masked by the 4/3-earth radar horizon
// (why sea-skimmers are seen late) and attenuated by rain. A successful
// detection yields a PLOT — a noisy spherical measurement converted to
// Cartesian with its full converted covariance attached, so the tracking
// filter's R is by construction the same model the radar actually used
// (charter §5.5 "모델과 필터의 정합성").
namespace seashield::sim {

struct RadarParams {
  double scan_period_s = 2.0;
  double antenna_height_m = 20.0;
  // Range at which SNR crosses the detection threshold: Pd = 0.5 in clear air.
  double reference_range_m = 30000.0;
  double pd_steepness_db = 3.0;  // Sigmoid slope in SNR dB.
  double sigma_range_m = 30.0;
  double sigma_az_rad = math::mrad_to_rad(3.0);
  double sigma_el_rad = math::mrad_to_rad(5.0);
  double rain_atten_db_per_km = 0.3;  // Two-way, at rain_intensity = 1.
};

// One radar measurement. position/covariance live in world ENU coordinates;
// the spherical fields keep the raw measurement for diagnostics and tests.
struct Plot {
  std::uint64_t tick = 0;
  std::uint32_t scan_index = 0;
  double range_m = 0.0;
  double azimuth_rad = 0.0;    // Clockwise from North (matches direction_from_az_el).
  double elevation_rad = 0.0;
  math::Vec3 position;         // Converted measurement, antenna offset applied.
  math::Mat3 covariance;       // J·diag(σ_r², σ_az², σ_el²)·Jᵀ at the measurement.
};

class Radar {
 public:
  // rain_intensity comes from the Weather (0..1). The RNG streams are split
  // (detection rolls vs measurement noise) so a change in one consumption
  // pattern can never shift the other.
  Radar(const RadarParams& params, double rain_intensity, std::uint64_t seed);

  // Advances one tick: every target whose azimuth bin matches this tick's
  // beam phase gets a detection opportunity. Draw discipline: one opportunity
  // ALWAYS consumes exactly 7 draws (1 Pd roll + 3 gaussians × 2), whether or
  // not it produces a plot — conditional consumption inside the subsystem is
  // where replay bugs breed. Targets must be passed in a fixed order (the
  // caller's determinism contract). Appends plots for this tick to out_plots
  // and to the current-scan buffer.
  // platform_position is the own ship's surface position; the antenna rides it
  // (mast height added on z). The default origin reproduces the legacy
  // fixed-mast geometry bit-for-bit.
  void step(std::uint64_t tick, std::span<const math::Vec3> target_positions,
            std::vector<Plot>& out_plots, const math::Vec3& platform_position = math::Vec3{});

  std::uint32_t scan_index(std::uint64_t tick) const {
    return static_cast<std::uint32_t>(tick / scan_ticks_);
  }
  std::uint64_t scan_ticks() const { return scan_ticks_; }

  // Pd(R) including the horizon cutoff — public so tests can pin the curve.
  // range_m is used for both SNR and horizon (slant ≈ ground at these
  // geometries; e.g. at 6 km / 300 m altitude the difference is ~7.5 m,
  // well inside σ_r = 30 m).
  double detection_probability(double range_m, double target_alt_m) const;
  double horizon_range_m(double target_alt_m) const;

  // Plots of the most recent scan (for the state hash and, later, PPI display).
  const std::vector<Plot>& last_scan_plots() const { return last_scan_plots_; }

  std::uint64_t detection_rng_state() const { return detection_rng_.state(); }
  std::uint64_t noise_rng_state() const { return noise_rng_.state(); }

 private:
  RadarParams params_;
  double rain_intensity_;
  std::uint64_t scan_ticks_;
  Pcg32 detection_rng_;  // Stream 12.
  Pcg32 noise_rng_;      // Stream 13.
  std::vector<Plot> last_scan_plots_;
};

}  // namespace seashield::sim
