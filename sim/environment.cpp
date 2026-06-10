#include "sim/environment.h"

#include <algorithm>
#include <cstdio>

namespace seashield::sim {

namespace {

constexpr double kKelvin = 273.15;
constexpr double kRDry = 287.058;    // J/(kg·K)
constexpr double kRVapor = 461.495;  // J/(kg·K)

// Magnus formula: saturation vapor pressure (Pa) at temperature (°C).
double saturation_vapor_pressure_pa(double temp_c) {
  return 610.94 * math::exp(17.625 * temp_c / (temp_c + 243.04));
}

}  // namespace

double Weather::surface_wind_speed() const {
  return wind_layers.empty() ? 0.0 : wind_layers.front().velocity.norm();
}

std::string Weather::describe() const {
  auto wind_text = [](const WindLayer& layer) {
    const double speed = layer.velocity.norm();
    // Meteorological convention: report the direction the wind blows FROM.
    double from_deg =
        math::rad_to_deg(math::atan2(-layer.velocity.x, -layer.velocity.y));
    if (from_deg < 0) {
      from_deg += 360.0;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1fm/s@%03.0f°", speed, from_deg);
    return std::string(buf);
  };

  const char* rain_text = rain_intensity <= 0.0   ? "없음"
                          : rain_intensity < 0.33 ? "약"
                          : rain_intensity < 0.66 ? "중"
                                                  : "강";
  const char* turb_text = turbulence_intensity < 0.12   ? "약"
                          : turbulence_intensity < 0.22 ? "중"
                                                        : "강";
  char buf[256];
  std::snprintf(buf, sizeof(buf), "기온 %.1f°C · 기압 %.0fhPa · 습도 %.0f%% · 강우 %s · 난류 %s",
                sea_level_temperature_c, sea_level_pressure_pa / 100.0, humidity * 100.0,
                rain_text, turb_text);
  std::string out(buf);
  if (!wind_layers.empty()) {
    out += " · 지상풍 " + wind_text(wind_layers.front());
    if (wind_layers.size() > 2) {
      const WindLayer& mid = wind_layers[wind_layers.size() / 2];
      std::snprintf(buf, sizeof(buf), " · %.1fkm풍 ", mid.altitude_m / 1000.0);
      out += buf + wind_text(mid);
    }
  }
  return out;
}

Atmosphere::Atmosphere(const Weather& weather)
    : t0_k_(weather.sea_level_temperature_c + kKelvin),
      p0_pa_(weather.sea_level_pressure_pa),
      lapse_k_per_m_(weather.lapse_rate_c_per_m),
      gravity_(weather.gravity_mps2),
      humidity_(weather.humidity) {}

double Atmosphere::temperature_c(double altitude_m) const {
  return (t0_k_ - lapse_k_per_m_ * altitude_m) - kKelvin;
}

double Atmosphere::pressure_pa(double altitude_m) const {
  if (lapse_k_per_m_ < 1e-6) {
    // Isothermal fallback (a scenario override may zero the lapse rate).
    return p0_pa_ * math::exp(-gravity_ * altitude_m / (kRDry * t0_k_));
  }
  const double t_k = std::max(1.0, t0_k_ - lapse_k_per_m_ * altitude_m);
  return p0_pa_ * math::pow(t_k / t0_k_, gravity_ / (kRDry * lapse_k_per_m_));
}

double Atmosphere::density(double altitude_m) const {
  const double t_k = std::max(1.0, t0_k_ - lapse_k_per_m_ * altitude_m);
  const double p = pressure_pa(altitude_m);
  // Constant relative humidity with altitude — a deliberate simplification.
  const double e = humidity_ * saturation_vapor_pressure_pa(t_k - kKelvin);
  const double e_capped = std::min(e, 0.99 * p);
  return (p - e_capped) / (kRDry * t_k) + e_capped / (kRVapor * t_k);
}

WindField::WindField(std::vector<WindLayer> layers) : layers_(std::move(layers)) {
  std::sort(layers_.begin(), layers_.end(),
            [](const WindLayer& a, const WindLayer& b) { return a.altitude_m < b.altitude_m; });
}

math::Vec3 WindField::wind_at(double altitude_m) const {
  if (layers_.empty()) {
    return {};
  }
  if (altitude_m <= layers_.front().altitude_m) {
    return layers_.front().velocity;
  }
  if (altitude_m >= layers_.back().altitude_m) {
    return layers_.back().velocity;
  }
  for (std::size_t i = 1; i < layers_.size(); ++i) {
    if (altitude_m <= layers_[i].altitude_m) {
      const WindLayer& lo = layers_[i - 1];
      const WindLayer& hi = layers_[i];
      const double t = (altitude_m - lo.altitude_m) / (hi.altitude_m - lo.altitude_m);
      // Component-wise lerp handles direction wrap-around naturally.
      return lo.velocity + (hi.velocity - lo.velocity) * t;
    }
  }
  return layers_.back().velocity;
}

GustProcess::GustProcess(double sigma_target_mps, std::uint64_t seed)
    // OU stationary std = sigma_ou / sqrt(2*theta)  =>  sigma_ou = target * sqrt(2/tau).
    : sigma_ou_(sigma_target_mps * math::sqrt(2.0 / kTimeConstantS)), rng_(seed, /*stream=*/3) {}

void GustProcess::step(double dt_s) {
  const double theta = 1.0 / kTimeConstantS;
  const double diffusion = sigma_ou_ * math::sqrt(dt_s);
  gust_.x += -theta * gust_.x * dt_s + diffusion * rng_.gaussian(0, 1);
  gust_.y += -theta * gust_.y * dt_s + diffusion * rng_.gaussian(0, 1);
  gust_.z += -theta * gust_.z * dt_s + diffusion * kVerticalScale * rng_.gaussian(0, 1);
}

Weather WeatherGenerator::generate(std::uint64_t weather_seed) {
  Pcg32 rng(weather_seed, /*stream=*/1);
  Weather w;
  w.sea_level_temperature_c = rng.uniform(-5.0, 32.0);
  w.sea_level_pressure_pa = rng.gaussian(101325.0, 800.0);
  w.lapse_rate_c_per_m = std::clamp(rng.gaussian(0.0065, 0.0008), 0.004, 0.0098);
  // Averaging two uniforms biases humidity toward the middle of the range.
  w.humidity = 0.5 * (rng.uniform(0.2, 0.95) + rng.uniform(0.2, 0.95));
  w.rain_intensity =
      w.humidity > 0.7 ? (w.humidity - 0.7) / 0.3 * rng.next_double() : 0.0;

  // Surface wind: skewed toward calm (u^2), 2..16 m/s at sea level.
  const double u = rng.next_double();
  const double surface_speed = 2.0 + 14.0 * u * u;
  double direction = rng.uniform(0.0, math::kTwoPi);  // Blowing-toward angle.

  // Wind grows with altitude (power law, open-sea exponent) and veers
  // (Ekman spiral) — correlated, realistic structure from one seed.
  static constexpr double kAltitudes[] = {0.0, 500.0, 1500.0, 3000.0, 6000.0};
  for (const double altitude : kAltitudes) {
    const double speed =
        std::min(35.0, surface_speed * math::pow(std::max(altitude, 10.0) / 10.0, 0.12));
    if (altitude > 0.0) {
      direction += math::deg_to_rad(rng.gaussian(8.0, 4.0));
    }
    w.wind_layers.push_back(
        {altitude, math::Vec3{math::sin(direction), math::cos(direction), 0.0} * speed});
  }

  w.turbulence_intensity =
      std::clamp(0.06 + surface_speed / 80.0 + 0.08 * w.rain_intensity, 0.05, 0.35);
  return w;
}

}  // namespace seashield::sim
