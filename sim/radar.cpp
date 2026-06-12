#include "sim/radar.h"

#include <algorithm>
#include <cmath>

#include "sim/constants.h"

namespace seashield::sim {
namespace {

// 4/3 effective earth radius: standard refraction model for the radar horizon.
constexpr double kEarthRadiusM = 6371000.0;
constexpr double kEffectiveEarthFactor = 4.0 / 3.0;
constexpr double kInvLn10 = 0.43429448190325182765;  // log10(x) = ln(x)·this.

// Distinct from kLaunchPosition on purpose: the antenna sits on the mast, the
// launcher on the deck. Mixing them up biases every elevation measurement.
math::Vec3 antenna_position(const RadarParams& params) {
  return {0.0, 0.0, params.antenna_height_m};
}

double wrap_two_pi(double angle_rad) {
  double wrapped = std::fmod(angle_rad, math::kTwoPi);
  if (wrapped < 0.0) {
    wrapped += math::kTwoPi;
  }
  return wrapped;
}

}  // namespace

Radar::Radar(const RadarParams& params, double rain_intensity, std::uint64_t seed)
    : params_(params),
      rain_intensity_(rain_intensity),
      scan_ticks_(static_cast<std::uint64_t>(
          std::max<long long>(1, std::llround(params.scan_period_s * kTickRateHz)))),
      detection_rng_(seed, 12),
      noise_rng_(seed, 13) {}

double Radar::horizon_range_m(double target_alt_m) const {
  const double k2re = 2.0 * kEffectiveEarthFactor * kEarthRadiusM;
  return math::sqrt(k2re * params_.antenna_height_m) +
         math::sqrt(k2re * std::max(0.0, target_alt_m));
}

double Radar::detection_probability(double range_m, double target_alt_m) const {
  if (range_m <= 0.0) {
    return 1.0;
  }
  if (range_m > horizon_range_m(target_alt_m)) {
    return 0.0;  // Geometric masking: below the horizon there is no echo at all.
  }
  // SNR ∝ 1/R⁴ → 40·log10(ref/R) dB relative to the detection threshold,
  // minus two-way rain attenuation along the path.
  const double snr_db = 40.0 * math::log(params_.reference_range_m / range_m) * kInvLn10 -
                        2.0 * (range_m / 1000.0) * params_.rain_atten_db_per_km * rain_intensity_;
  return 1.0 / (1.0 + math::exp(-snr_db / params_.pd_steepness_db));
}

void Radar::step(std::uint64_t tick, std::span<const math::Vec3> target_positions,
                 std::vector<Plot>& out_plots) {
  const std::uint64_t beam_phase = tick % scan_ticks_;
  const std::uint32_t scan = scan_index(tick);
  if (!last_scan_plots_.empty() && last_scan_plots_.front().scan_index != scan) {
    last_scan_plots_.clear();
  }

  for (const math::Vec3& truth : target_positions) {
    const math::Vec3 rel = truth - antenna_position(params_);
    const double ground_range = math::sqrt(rel.x * rel.x + rel.y * rel.y);
    const double true_azimuth = wrap_two_pi(math::atan2(rel.x, rel.y));  // CW from North.

    // Integer beam phase: the target's azimuth bin must equal this tick's
    // phase. Derived from the tick counter — no floating-point accumulation,
    // exactly one opportunity per scan, nothing extra to hash.
    auto bin = static_cast<std::uint64_t>(true_azimuth / math::kTwoPi *
                                          static_cast<double>(scan_ticks_));
    bin = std::min(bin, scan_ticks_ - 1);  // az == 2π guard.
    if (bin != beam_phase) {
      continue;
    }

    const double slant_range = rel.norm();

    // Fixed 7-draw opportunity (see header): roll first, noise always.
    // The noisy angles are stored unwrapped (diagnostic fields only): every
    // load-bearing consumer goes through the Cartesian conversion, which is
    // periodic in the angle.
    const double roll = detection_rng_.next_double();
    const double noisy_range = slant_range + noise_rng_.gaussian(0.0, params_.sigma_range_m);
    const double noisy_azimuth = true_azimuth + noise_rng_.gaussian(0.0, params_.sigma_az_rad);
    const double noisy_elevation =
        math::atan2(rel.z, ground_range) + noise_rng_.gaussian(0.0, params_.sigma_el_rad);

    if (roll >= detection_probability(slant_range, truth.z)) {
      continue;  // Missed: the draws above were still consumed (fixed budget).
    }
    if (noisy_range <= 0.0) {
      continue;  // Degenerate measurement; discard rather than emit garbage.
    }

    Plot plot;
    plot.tick = tick;
    plot.scan_index = scan;
    plot.range_m = noisy_range;
    plot.azimuth_rad = noisy_azimuth;
    plot.elevation_rad = noisy_elevation;
    plot.position =
        antenna_position(params_) +
        math::direction_from_az_el(noisy_azimuth, noisy_elevation) * noisy_range;

    // Converted-measurement covariance: R = J·diag(σ_r², σ_az², σ_el²)·Jᵀ
    // with the Jacobian of (r, az, el) → ENU evaluated AT the measurement.
    //   x = r·sin(az)·cos(el),  y = r·cos(az)·cos(el),  z = r·sin(el)
    const double sin_az = math::sin(noisy_azimuth);
    const double cos_az = math::cos(noisy_azimuth);
    const double sin_el = math::sin(noisy_elevation);
    const double cos_el = math::cos(noisy_elevation);
    const double r = noisy_range;
    math::Mat3 jacobian;
    jacobian(0, 0) = sin_az * cos_el;
    jacobian(0, 1) = r * cos_az * cos_el;
    jacobian(0, 2) = -r * sin_az * sin_el;
    jacobian(1, 0) = cos_az * cos_el;
    jacobian(1, 1) = -r * sin_az * cos_el;
    jacobian(1, 2) = -r * cos_az * sin_el;
    jacobian(2, 0) = sin_el;
    jacobian(2, 1) = 0.0;
    jacobian(2, 2) = r * cos_el;
    math::Mat3 spherical;
    spherical(0, 0) = params_.sigma_range_m * params_.sigma_range_m;
    spherical(1, 1) = params_.sigma_az_rad * params_.sigma_az_rad;
    spherical(2, 2) = params_.sigma_el_rad * params_.sigma_el_rad;
    plot.covariance = jacobian * spherical * jacobian.transposed();

    out_plots.push_back(plot);
    last_scan_plots_.push_back(plot);
  }
}

}  // namespace seashield::sim
