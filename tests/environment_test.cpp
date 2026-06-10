#include "sim/environment.h"

#include <cmath>

#include <gtest/gtest.h>

namespace seashield::sim {
namespace {

Weather standard_weather() {
  Weather w;
  w.sea_level_temperature_c = 15.0;
  w.sea_level_pressure_pa = 101325.0;
  w.lapse_rate_c_per_m = 0.0065;
  w.humidity = 0.0;
  return w;
}

TEST(AtmosphereTest, SeaLevelDensityMatchesIsa) {
  const Atmosphere atm(standard_weather());
  // ISA sea-level density: 1.225 kg/m^3.
  EXPECT_NEAR(atm.density(0.0), 1.225, 0.005);
}

TEST(AtmosphereTest, DensityDecreasesWithAltitude) {
  const Atmosphere atm(standard_weather());
  double prev = atm.density(0.0);
  for (double h = 500; h <= 6000; h += 500) {
    const double d = atm.density(h);
    EXPECT_LT(d, prev) << "at altitude " << h;
    prev = d;
  }
  // ISA density at 3000m is ~0.909 kg/m^3.
  EXPECT_NEAR(atm.density(3000.0), 0.909, 0.01);
}

TEST(AtmosphereTest, HumidAirIsLessDense) {
  Weather dry = standard_weather();
  Weather humid = standard_weather();
  humid.humidity = 1.0;
  EXPECT_LT(Atmosphere(humid).density(0.0), Atmosphere(dry).density(0.0));
}

TEST(AtmosphereTest, ZeroLapseRateIsothermalFallback) {
  Weather w = standard_weather();
  w.lapse_rate_c_per_m = 0.0;
  const Atmosphere atm(w);
  EXPECT_TRUE(std::isfinite(atm.density(5000.0)));
  EXPECT_LT(atm.density(5000.0), atm.density(0.0));
}

TEST(WindFieldTest, InterpolatesBetweenLayers) {
  const WindField field({{0.0, {0, 10, 0}}, {1000.0, {10, 10, 0}}});
  const math::Vec3 mid = field.wind_at(500.0);
  EXPECT_NEAR(mid.x, 5.0, 1e-9);
  EXPECT_NEAR(mid.y, 10.0, 1e-9);
}

TEST(WindFieldTest, ClampsBeyondEnds) {
  const WindField field({{100.0, {1, 0, 0}}, {1000.0, {5, 0, 0}}});
  EXPECT_NEAR(field.wind_at(0.0).x, 1.0, 1e-9);
  EXPECT_NEAR(field.wind_at(9999.0).x, 5.0, 1e-9);
}

TEST(GustProcessTest, DeterministicForSameSeed) {
  GustProcess a(1.5, 42);
  GustProcess b(1.5, 42);
  for (int i = 0; i < 200; ++i) {
    a.step(1.0 / 60.0);
    b.step(1.0 / 60.0);
  }
  EXPECT_DOUBLE_EQ(a.current().x, b.current().x);
  EXPECT_DOUBLE_EQ(a.current().z, b.current().z);
}

TEST(GustProcessTest, StationaryStatisticsNearTarget) {
  constexpr double kSigma = 2.0;
  GustProcess gust(kSigma, 7);
  const double dt = 1.0 / 60.0;
  // Burn-in to reach the stationary distribution.
  for (int i = 0; i < 2000; ++i) {
    gust.step(dt);
  }
  double sum = 0;
  double sum_sq = 0;
  constexpr int kSamples = 200000;
  for (int i = 0; i < kSamples; ++i) {
    gust.step(dt);
    sum += gust.current().x;
    sum_sq += gust.current().x * gust.current().x;
  }
  const double mean = sum / kSamples;
  const double std = std::sqrt(sum_sq / kSamples - mean * mean);
  EXPECT_NEAR(mean, 0.0, 0.15);
  EXPECT_NEAR(std, kSigma, kSigma * 0.3);  // OU samples are correlated: loose tolerance.
}

TEST(WeatherGeneratorTest, SameSeedSameWeather) {
  const Weather a = WeatherGenerator::generate(123);
  const Weather b = WeatherGenerator::generate(123);
  EXPECT_DOUBLE_EQ(a.sea_level_temperature_c, b.sea_level_temperature_c);
  EXPECT_DOUBLE_EQ(a.turbulence_intensity, b.turbulence_intensity);
  ASSERT_EQ(a.wind_layers.size(), b.wind_layers.size());
  EXPECT_DOUBLE_EQ(a.wind_layers.back().velocity.x, b.wind_layers.back().velocity.x);
  EXPECT_EQ(a.describe(), b.describe());
}

TEST(WeatherGeneratorTest, RealisticRangesAcrossSeeds) {
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    const Weather w = WeatherGenerator::generate(seed);
    EXPECT_GE(w.sea_level_temperature_c, -5.0);
    EXPECT_LE(w.sea_level_temperature_c, 32.0);
    EXPECT_GE(w.humidity, 0.0);
    EXPECT_LE(w.humidity, 1.0);
    EXPECT_GE(w.rain_intensity, 0.0);
    EXPECT_LE(w.rain_intensity, 1.0);
    if (w.humidity <= 0.7) {
      EXPECT_EQ(w.rain_intensity, 0.0);  // Rain requires high humidity.
    }
    EXPECT_GE(w.turbulence_intensity, 0.05);
    EXPECT_LE(w.turbulence_intensity, 0.35);
    ASSERT_GE(w.wind_layers.size(), 2u);
    for (std::size_t i = 1; i < w.wind_layers.size(); ++i) {
      EXPECT_GT(w.wind_layers[i].altitude_m, w.wind_layers[i - 1].altitude_m);
      // Wind speed grows (or stays capped) with altitude.
      EXPECT_GE(w.wind_layers[i].velocity.norm() + 1e-9, w.wind_layers[i - 1].velocity.norm());
    }
    EXPECT_LE(w.wind_layers.back().velocity.norm(), 35.0 + 1e-9);
  }
}

}  // namespace
}  // namespace seashield::sim
