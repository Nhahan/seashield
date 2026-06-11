#include "sim/fire_control.h"

#include <gtest/gtest.h>

#include "sim/target.h"
#include "sim/world.h"

namespace seashield::sim {
namespace {

Weather calm_weather() {
  Weather w;
  w.humidity = 0.0;
  w.rain_intensity = 0.0;
  w.turbulence_intensity = 0.0;
  w.wind_layers = {{0.0, {0, 0, 0}}};
  return w;
}

Weather crosswind_weather(double speed_mps) {
  Weather w = calm_weather();
  w.wind_layers = {{0.0, {speed_mps, 0, 0}}, {6000.0, {speed_mps, 0, 0}}};
  return w;
}

TargetParams crossing_target() {
  TargetParams t;
  t.initial_position = {4000, 3000, 600};
  t.heading_rad = math::deg_to_rad(270.0);  // Westbound.
  t.speed_mps = 200.0;
  t.turn_rate_rad_s = 0.0;
  return t;
}

// Closed loop: solve, fire in the world with zero dispersion, expect a fuze
// detonation. Returns the recorded miss distance.
double closed_loop_miss(const Weather& weather, const TargetParams& target_params,
                        bool* detonated) {
  const Target probe(target_params);
  const FireControlSolver solver(weather, RocketParams{});
  const auto solution = solver.solve(target_params.initial_position, probe.velocity());
  EXPECT_TRUE(solution.has_value());
  if (!solution.has_value()) {
    *detonated = false;
    return 1e30;
  }
  EXPECT_LT(solution->predicted_miss_m, 1.0);

  WorldConfig cfg;
  cfg.weather = weather;
  cfg.target = target_params;
  World world(cfg);
  FireCommand cmd;
  cmd.azimuth_rad = solution->azimuth_rad;
  cmd.elevation_rad = solution->elevation_rad;
  cmd.salvo_count = 1;
  cmd.dispersion_mrad = 0.0;
  world.queue_fire(cmd);
  while (!world.ordnance_resolved() && world.tick() < 60 * 60) {
    world.step();
  }
  EXPECT_EQ(world.results().size(), 1u);
  *detonated = !world.results().empty() && world.results()[0].detonated;
  return world.results().empty() ? 1e30 : world.results()[0].miss_distance_m;
}

TEST(FireControlTest, CalmStraightCrossingTargetIsHit) {
  bool detonated = false;
  const double miss = closed_loop_miss(calm_weather(), crossing_target(), &detonated);
  EXPECT_TRUE(detonated);
  EXPECT_LT(miss, 12.0);
}

TEST(FireControlTest, CrosswindIsCompensatedAndStillHits) {
  const Target probe(crossing_target());
  const auto calm_solution =
      FireControlSolver(calm_weather(), RocketParams{})
          .solve(crossing_target().initial_position, probe.velocity());
  const auto wind_solution =
      FireControlSolver(crosswind_weather(12.0), RocketParams{})
          .solve(crossing_target().initial_position, probe.velocity());
  ASSERT_TRUE(calm_solution.has_value());
  ASSERT_TRUE(wind_solution.has_value());
  // The solver leans into the wind: the firing azimuth must shift.
  const double shift = wind_solution->azimuth_rad - calm_solution->azimuth_rad;
  EXPECT_GT(math::sqrt(shift * shift), math::deg_to_rad(0.1));

  bool detonated = false;
  const double miss = closed_loop_miss(crosswind_weather(12.0), crossing_target(), &detonated);
  EXPECT_TRUE(detonated);
  EXPECT_LT(miss, 12.0);
}

TEST(FireControlTest, TurningTargetBreaksConstantVelocityPrediction) {
  TargetParams turning = crossing_target();
  turning.turn_rate_rad_s = math::deg_to_rad(6.0);
  bool detonated = false;
  const double miss = closed_loop_miss(calm_weather(), turning, &detonated);
  // The CV extrapolation is invalidated by the maneuver: a clean miss.
  // This is the quantified limitation of unguided fire (charter §2.4).
  EXPECT_GT(miss, 30.0);
}

// Regression: a target closing on the launcher makes the PIP fixed-point
// oscillate if the solver drifts onto the lofted arc — it must stay on the
// direct (minimum-time) arc and converge (demo scenario geometry).
TEST(FireControlTest, ClosingTargetConvergesOnDirectArcAndHits) {
  TargetParams closing;
  closing.initial_position = {6000, 8000, 800};
  closing.heading_rad = math::deg_to_rad(225.0);  // Straight at the ship.
  closing.speed_mps = 250.0;
  closing.turn_rate_rad_s = 0.0;

  const Target probe(closing);
  const auto solution = FireControlSolver(calm_weather(), RocketParams{})
                            .solve(closing.initial_position, probe.velocity());
  ASSERT_TRUE(solution.has_value());
  // Direct arc for this geometry is ~18s; the oscillating lofted branch the
  // solver must avoid sits at 25s+.
  EXPECT_LT(solution->time_of_flight_s, 22.0);

  bool detonated = false;
  const double miss = closed_loop_miss(calm_weather(), closing, &detonated);
  EXPECT_TRUE(detonated);
  EXPECT_LT(miss, 12.0);
}

TEST(FireControlTest, UnreachableTargetReturnsNoSolution) {
  const FireControlSolver solver(calm_weather(), RocketParams{});
  EXPECT_FALSE(solver.solve({40000, 0, 9000}, {0, 0, 0}).has_value());
}

// P4: the same engagement fired from TRUTH vs from the ESTIMATED track.
// Identical seeds make both runs reach the identical world state at the solve
// tick, so the miss difference is purely the sensor/tracking error propagating
// through the solver (charter §5.6 항목 1). Statistical sweeps over many
// seeds belong to the experiment harness; this is the single-seed regression.
TEST(FireControlTest, EstimateFedSolutionDegradesGracefully) {
  WorldConfig cfg;
  cfg.weather = calm_weather();
  cfg.target = crossing_target();
  cfg.radar.scan_period_s = 0.5;  // Fast scans: confirmed track within ~2 s.

  enum class Source { kTruth, kTrack };
  const auto run_engagement = [&](Source source, double* out_miss) -> bool {
    World world(cfg);
    // Deterministic solve tick: first tick with a confirmed track that has
    // seen at least 10 scans (settled velocity estimate).
    std::uint32_t scans_after_confirm = 0;
    while (world.tick() < 60 * 30) {
      world.step();
      const auto& tracks = world.tracker().tracks();
      if (!tracks.empty() && tracks[0].status == TrackStatus::kConfirmed &&
          ++scans_after_confirm >= 10 * 30) {  // ~10 scans at 0.5 s in ticks.
        break;
      }
    }
    const auto& tracks = world.tracker().tracks();
    if (tracks.empty() || tracks[0].status != TrackStatus::kConfirmed) {
      return false;
    }

    std::optional<FiringSolution> solution;
    if (source == Source::kTruth) {
      const FireControlSolver solver(cfg.weather, cfg.rocket);
      solution = solver.solve(world.target().position(), world.target().velocity());
    } else {
      solution = world.solve_for_track(tracks[0].id);
    }
    if (!solution.has_value()) {
      return false;
    }

    FireCommand cmd;
    cmd.azimuth_rad = solution->azimuth_rad;
    cmd.elevation_rad = solution->elevation_rad;
    cmd.salvo_count = 1;
    cmd.dispersion_mrad = 0.0;  // Isolate the aiming error from launcher scatter.
    world.queue_fire(cmd);
    while (!world.ordnance_resolved() && world.tick() < 60 * 60) {
      world.step();
    }
    if (world.results().empty()) {
      return false;
    }
    *out_miss = world.results()[0].miss_distance_m;
    return true;
  };

  double truth_miss = 0.0;
  double track_miss = 0.0;
  ASSERT_TRUE(run_engagement(Source::kTruth, &truth_miss));
  ASSERT_TRUE(run_engagement(Source::kTrack, &track_miss));

  // Truth aim on a non-maneuvering target in calm air is near-perfect.
  EXPECT_LT(truth_miss, 12.0);
  // The estimate-fed shot is worse — that is the point — but bounded: the
  // tracking error must not blow the shot into the next county.
  EXPECT_LT(track_miss, 250.0);
  EXPECT_LE(truth_miss, track_miss + 1.0)
      << "estimated track outperformed truth — check the harness, not the luck";
}

}  // namespace
}  // namespace seashield::sim
