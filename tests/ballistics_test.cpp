#include "sim/ballistics.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "sim/environment.h"

namespace seashield::sim {
namespace {

constexpr double kDt = 1.0 / 60.0;

Weather dry_standard_weather() {
  Weather w;
  w.sea_level_temperature_c = 15.0;
  w.sea_level_pressure_pa = 101325.0;
  w.lapse_rate_c_per_m = 0.0065;
  w.humidity = 0.0;
  return w;
}

RocketParams no_thrust_params() {
  RocketParams p;
  p.thrust_n = 0.0;
  p.burn_time_s = 0.0;
  return p;
}

TEST(BallisticsTest, VacuumParabolaMatchesAnalyticSolution) {
  RocketParams params = no_thrust_params();
  FlightEnvironment env;  // atmosphere == nullptr → no drag.
  const math::Vec3 dir = math::direction_from_az_el(math::deg_to_rad(90), math::deg_to_rad(45));
  RocketState s;
  s.velocity = dir * 100.0;
  const math::Vec3 v0 = s.velocity;

  constexpr int kSteps = 120;  // 2 seconds.
  for (int i = 0; i < kSteps; ++i) {
    rocket_step(s, params, env, dir, kDt);
  }
  const double t = kSteps * kDt;
  // Constant acceleration: RK4 reproduces the parabola to round-off.
  EXPECT_NEAR(s.position.x, v0.x * t, 1e-7);
  EXPECT_NEAR(s.position.y, v0.y * t, 1e-7);
  EXPECT_NEAR(s.position.z, v0.z * t - 0.5 * env.gravity_mps2 * t * t, 1e-7);
}

TEST(BallisticsTest, BoostAcceleratesPastLaunchSpeed) {
  RocketParams params;  // Default: 18kN for 1.5s on a 60kg rocket.
  params.cda_m2 = 0.0;  // Isolate the thrust term.
  FlightEnvironment env;
  const math::Vec3 dir = math::direction_from_az_el(0.0, math::deg_to_rad(45));
  RocketState s;
  s.velocity = dir * params.rail_exit_speed_mps;
  while (s.age_s < params.burn_time_s) {
    rocket_step(s, params, env, dir, kDt);
  }
  // Δv ≈ (18000/60)·1.5 = 450 m/s minus gravity losses.
  EXPECT_GT(s.velocity.norm(), 300.0);
}

TEST(BallisticsTest, FallingObjectReachesTerminalVelocity) {
  const Weather weather = dry_standard_weather();
  const Atmosphere atmosphere(weather);
  FlightEnvironment env;
  env.atmosphere = &atmosphere;

  RocketParams params = no_thrust_params();
  params.mass_kg = 1.0;
  params.cda_m2 = 0.5;  // Terminal velocity ≈ 5.7 m/s — reached quickly.

  RocketState s;
  s.position = {0, 0, 300.0};
  const math::Vec3 down{0, 0, -1};
  for (int i = 0; i < 4000 && s.position.z > 5.0; ++i) {
    rocket_step(s, params, env, down, kDt);
  }
  const double rho = atmosphere.density(s.position.z);
  const double terminal =
      math::sqrt(2.0 * params.mass_kg * env.gravity_mps2 / (rho * params.cda_m2));
  EXPECT_NEAR(s.velocity.norm(), terminal, terminal * 0.02);
}

TEST(BallisticsTest, CrosswindDriftsImpactDownwind) {
  const Weather weather = dry_standard_weather();
  const Atmosphere atmosphere(weather);
  const WindField crosswind({{0.0, {10, 0, 0}}, {6000.0, {10, 0, 0}}});

  const auto impact_x = [&](const WindField* wind) {
    FlightEnvironment env;
    env.atmosphere = &atmosphere;
    env.wind = wind;
    RocketParams params = no_thrust_params();
    const math::Vec3 north = math::direction_from_az_el(0.0, math::deg_to_rad(45));
    RocketState s;
    s.velocity = north * 300.0;
    s.position = {0, 0, 1.0};
    while (s.position.z > 0.0 && s.age_s < 60.0) {
      rocket_step(s, params, env, north, kDt);
    }
    return s.position.x;
  };

  EXPECT_NEAR(impact_x(nullptr), 0.0, 0.5);     // Calm: no lateral drift.
  EXPECT_GT(impact_x(&crosswind), 5.0);          // +x wind pushes impact east.
}

TEST(BallisticsTest, HalfStepRefinementBarelyMoves) {
  const Weather weather = dry_standard_weather();
  const Atmosphere atmosphere(weather);
  FlightEnvironment env;
  env.atmosphere = &atmosphere;
  RocketParams params = no_thrust_params();
  const math::Vec3 dir = math::direction_from_az_el(math::deg_to_rad(30), math::deg_to_rad(40));

  RocketState coarse;
  coarse.velocity = dir * 300.0;
  RocketState fine = coarse;
  for (int i = 0; i < 120; ++i) {
    rocket_step(coarse, params, env, dir, kDt);
    rocket_step(fine, params, env, dir, kDt * 0.5);
    rocket_step(fine, params, env, dir, kDt * 0.5);
  }
  EXPECT_NEAR((coarse.position - fine.position).norm(), 0.0, 1e-6);
}

TEST(BallisticsTest, MatchesIndependentPythonReference) {
  // Golden values from tools/reference/ballistics_ref.py — an independent
  // implementation of the same model in another language.
  std::ifstream golden(std::string(SEASHIELD_SOURCE_DIR) + "/tests/golden/ballistics_ref.csv");
  ASSERT_TRUE(golden.is_open()) << "run tools/reference/ballistics_ref.py first";

  struct Sample {
    int step;
    math::Vec3 position;
  };
  std::vector<Sample> samples;
  std::string line;
  std::getline(golden, line);  // Header.
  while (std::getline(golden, line)) {
    std::istringstream ss(line);
    std::string field;
    Sample sample{};
    std::getline(ss, field, ',');
    sample.step = std::stoi(field);
    std::getline(ss, field, ',');
    sample.position.x = std::stod(field);
    std::getline(ss, field, ',');
    sample.position.y = std::stod(field);
    std::getline(ss, field, ',');
    sample.position.z = std::stod(field);
    samples.push_back(sample);
  }
  ASSERT_FALSE(samples.empty());

  const Weather weather = dry_standard_weather();
  const Atmosphere atmosphere(weather);
  FlightEnvironment env;
  env.atmosphere = &atmosphere;
  RocketParams params = no_thrust_params();  // mass 60, CdA 0.012 — same as script.
  const math::Vec3 dir = math::direction_from_az_el(math::deg_to_rad(90), math::deg_to_rad(30));
  RocketState s;
  s.velocity = dir * 300.0;

  int step = 0;
  for (const Sample& sample : samples) {
    for (; step < sample.step; ++step) {
      rocket_step(s, params, env, dir, kDt);
    }
    EXPECT_NEAR(s.position.x, sample.position.x, 1e-3) << "step " << sample.step;
    EXPECT_NEAR(s.position.y, sample.position.y, 1e-3) << "step " << sample.step;
    EXPECT_NEAR(s.position.z, sample.position.z, 1e-3) << "step " << sample.step;
  }
}

}  // namespace
}  // namespace seashield::sim
