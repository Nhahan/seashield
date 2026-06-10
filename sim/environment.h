#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/math.h"
#include "core/pcg32.h"

// Environment model (charter §5.6 "환경 모델 명세"). The weather state is
// produced by WeatherGenerator from a single seed — realistic, correlated,
// and bit-reproducible — and consumed by the ballistics through Atmosphere
// (density), WindField (mean wind) and GustProcess (turbulence).
namespace seashield::sim {

struct WindLayer {
  double altitude_m = 0.0;
  math::Vec3 velocity;  // Horizontal wind vector: the direction air moves TOWARD.
};

struct Weather {
  double sea_level_temperature_c = 15.0;
  double sea_level_pressure_pa = 101325.0;
  double lapse_rate_c_per_m = 0.0065;
  double humidity = 0.5;              // Relative humidity, 0..1.
  double rain_intensity = 0.0;        // 0 = none .. 1 = heavy.
  double turbulence_intensity = 0.1;  // Gust sigma as a fraction of mean wind speed.
  double gravity_mps2 = 9.80665;
  std::vector<WindLayer> wind_layers;  // Sorted by ascending altitude.

  double surface_wind_speed() const;
  // Human-readable summary for the sandbox CLI (wind reported in the
  // meteorological "blowing from" convention).
  std::string describe() const;
};

// Simplified ISA: linear temperature lapse, barometric pressure, ideal-gas
// density with a humidity (vapor partial pressure, Magnus formula) correction.
class Atmosphere {
 public:
  explicit Atmosphere(const Weather& weather);

  double temperature_c(double altitude_m) const;
  double pressure_pa(double altitude_m) const;
  double density(double altitude_m) const;  // kg/m^3

 private:
  double t0_k_;
  double p0_pa_;
  double lapse_k_per_m_;
  double gravity_;
  double humidity_;
};

// Mean wind by altitude: component-wise linear interpolation between layers,
// clamped at both ends (no extrapolation).
class WindField {
 public:
  explicit WindField(std::vector<WindLayer> layers);
  math::Vec3 wind_at(double altitude_m) const;

 private:
  std::vector<WindLayer> layers_;
};

// Gust/turbulence ("기류") as a per-axis Ornstein-Uhlenbeck process with a
// fixed seed: deterministic, zero-mean, with stationary standard deviation
// sigma_target on the horizontal axes (damped on the vertical axis).
//
// The fire-control solution can only compensate the MEAN wind; gusts remain
// as irreducible error — the physical reason salvo fire exists (charter §5.6).
class GustProcess {
 public:
  GustProcess(double sigma_target_mps, std::uint64_t seed);

  void step(double dt_s);
  const math::Vec3& current() const { return gust_; }
  std::uint64_t rng_state() const { return rng_.state(); }

 private:
  static constexpr double kTimeConstantS = 2.0;
  static constexpr double kVerticalScale = 0.3;

  double sigma_ou_;
  Pcg32 rng_;
  math::Vec3 gust_;
};

// One seed -> a full, realistically correlated weather state: wind grows and
// veers with altitude (power law / Ekman veer), turbulence scales with wind
// and rain, rain requires high humidity.
struct WeatherGenerator {
  static Weather generate(std::uint64_t weather_seed);
};

}  // namespace seashield::sim
