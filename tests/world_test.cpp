#include "sim/world.h"

#include <gtest/gtest.h>

namespace seashield::sim {
namespace {

Weather calm_weather() {
  Weather w;
  w.humidity = 0.0;
  w.turbulence_intensity = 0.0;
  w.rain_intensity = 0.0;
  w.wind_layers = {{0.0, {0, 0, 0}}};
  return w;
}

WorldConfig hovering_target_config() {
  WorldConfig cfg;
  cfg.weather = calm_weather();
  cfg.target.initial_position = {0, 0, 2000};
  cfg.target.speed_mps = 0.0;
  cfg.target.turn_rate_rad_s = 0.0;
  return cfg;
}

TEST(WorldTest, SensorChainConfirmsTrackOnInboundTarget) {
  WorldConfig cfg;
  cfg.weather = calm_weather();
  cfg.target.initial_position = {6000, 8000, 800};
  cfg.target.heading_rad = math::deg_to_rad(225.0);
  cfg.target.speed_mps = 250.0;
  cfg.radar.scan_period_s = 0.5;  // Fast scans keep the test short.
  World world(cfg);

  std::uint32_t confirmed_id = 0;
  for (int i = 0; i < 60 * 20 && confirmed_id == 0; ++i) {
    world.step();
    for (const TrackEvent& event : world.track_events()) {
      if (event.kind == TrackEvent::Kind::kConfirmed) {
        confirmed_id = event.track_id;
      }
    }
  }
  ASSERT_NE(confirmed_id, 0u) << "no track confirmed within 20 s";

  const Track* track = world.tracker().find(confirmed_id);
  ASSERT_NE(track, nullptr);
  EXPECT_EQ(track->status, TrackStatus::kConfirmed);
  // The estimate follows the truth (generous bound: a few sigma of the
  // converted measurement at ~10 km).
  EXPECT_LT((track->position() - world.target().position()).norm(), 300.0);

  // Lifecycle log is tick-ordered and starts with an initiation.
  ASSERT_FALSE(world.track_events().empty());
  EXPECT_EQ(world.track_events().front().kind, TrackEvent::Kind::kInitiated);
  for (std::size_t i = 1; i < world.track_events().size(); ++i) {
    EXPECT_LE(world.track_events()[i - 1].tick, world.track_events()[i].tick);
  }
}

TEST(WorldTest, SolveForTrackEnforcesConfirmationAndLeavesStateUntouched) {
  WorldConfig cfg;
  cfg.weather = calm_weather();
  cfg.target.initial_position = {4000, 3000, 600};
  cfg.target.heading_rad = math::deg_to_rad(270.0);
  cfg.target.speed_mps = 200.0;
  cfg.radar.scan_period_s = 0.5;
  World world(cfg);

  World::SolveForTrackError reason;
  EXPECT_FALSE(world.solve_for_track(99, &reason).has_value());
  EXPECT_EQ(reason, World::SolveForTrackError::kNoSuchTrack);

  // Step until the first TENTATIVE track exists: firing on it is refused.
  while (world.tracker().tracks().empty() && world.tick() < 60 * 10) {
    world.step();
  }
  ASSERT_FALSE(world.tracker().tracks().empty());
  const std::uint32_t id = world.tracker().tracks()[0].id;
  if (world.tracker().tracks()[0].status == TrackStatus::kTentative) {
    EXPECT_FALSE(world.solve_for_track(id, &reason).has_value());
    EXPECT_EQ(reason, World::SolveForTrackError::kNotConfirmed);
  }

  // Step until confirmed, then solve: a solution exists and the call is
  // PURE — replay safety hinges on this (charter §5.8).
  while (world.tracker().find(id) != nullptr &&
         world.tracker().find(id)->status != TrackStatus::kConfirmed &&
         world.tick() < 60 * 20) {
    world.step();
  }
  ASSERT_NE(world.tracker().find(id), nullptr);
  ASSERT_EQ(world.tracker().find(id)->status, TrackStatus::kConfirmed);

  const std::uint64_t hash_before = world.state_hash();
  const auto solution = world.solve_for_track(id, &reason);
  EXPECT_EQ(world.state_hash(), hash_before) << "solve_for_track mutated world state";
  ASSERT_TRUE(solution.has_value());
  EXPECT_GT(solution->time_of_flight_s, 0.0);
}

// P5: a confirmed track that keeps coasting (here: a sea-skimmer sailing past
// the radar horizon) is refused for firing once it exceeds max_coast_scans —
// the CV extrapolation is too stale to aim with, yet the track still lives.
TEST(WorldTest, StaleCoastingTrackIsRefusedForFiring) {
  WorldConfig cfg;
  cfg.weather = calm_weather();
  cfg.radar.antenna_height_m = 1.0;  // Horizon ≈ 13.3 km against a 5 m target.
  cfg.radar.scan_period_s = 0.5;
  cfg.tracker.drop_after_misses = 10;  // Keep the track alive while it coasts.
  cfg.target.initial_position = {0, 5000, 5};
  cfg.target.heading_rad = 0.0;  // Due north: outbound, toward the horizon.
  cfg.target.speed_mps = 300.0;
  World world(cfg);

  std::uint32_t id = 0;
  while (id == 0 && world.tick() < 60 * 20) {
    world.step();
    for (const TrackEvent& event : world.track_events()) {
      if (event.kind == TrackEvent::Kind::kConfirmed) {
        id = event.track_id;
      }
    }
  }
  ASSERT_NE(id, 0u) << "no track confirmed before the target left coverage";

  // The gate trips BEFORE the solver runs, so this probe is cheap.
  World::SolveForTrackError reason{};
  bool stale_seen = false;
  while (!stale_seen && world.tick() < 60 * 60) {
    world.step();
    const Track* track = world.tracker().find(id);
    ASSERT_NE(track, nullptr) << "track dropped before the staleness gate tripped";
    if (track->consecutive_missed_scans >= cfg.tracker.max_coast_scans) {
      EXPECT_FALSE(world.solve_for_track(id, &reason).has_value());
      EXPECT_EQ(reason, World::SolveForTrackError::kStale);
      EXPECT_EQ(track->status, TrackStatus::kConfirmed);  // Coasting, not dropped.
      EXPECT_TRUE(track->coasting());
      stale_seen = true;
    }
  }
  EXPECT_TRUE(stale_seen) << "target never coasted past the gate within 60 s";
}

TEST(WorldTest, SalvoLaunchesRequestedCount) {
  World world(hovering_target_config());
  FireCommand cmd;
  cmd.salvo_count = 4;
  cmd.dispersion_mrad = 3.0;
  world.queue_fire(cmd);
  for (int i = 0; i < 60; ++i) {
    world.step();
  }
  EXPECT_EQ(world.rockets().size(), 4u);
}

TEST(WorldTest, VerticalShotDetonatesOnHoveringTarget) {
  World world(hovering_target_config());
  FireCommand cmd;
  cmd.azimuth_rad = 0.0;
  cmd.elevation_rad = math::deg_to_rad(90.0);
  cmd.salvo_count = 1;
  cmd.dispersion_mrad = 0.0;  // Perfect alignment: straight through the target.
  world.queue_fire(cmd);

  while (!world.ordnance_resolved() && world.tick() < 60 * 60) {
    world.step();
  }
  ASSERT_EQ(world.results().size(), 1u);
  const RocketResult& result = world.results()[0];
  EXPECT_TRUE(result.detonated);
  EXPECT_LT(result.miss_distance_m, 12.0);
  EXPECT_EQ(result.killed, world.target().destroyed());
  EXPECT_FALSE(world.events().empty());
}

TEST(WorldTest, AllOrdnanceEventuallyResolves) {
  World world(hovering_target_config());
  FireCommand cmd;
  cmd.salvo_count = 6;
  cmd.elevation_rad = math::deg_to_rad(40.0);  // Misses the overhead target.
  cmd.dispersion_mrad = 5.0;
  world.queue_fire(cmd);

  while (!world.ordnance_resolved() && world.tick() < 60 * 120) {
    world.step();
  }
  EXPECT_TRUE(world.ordnance_resolved());
  EXPECT_EQ(world.results().size(), 6u);
  for (const RocketResult& r : world.results()) {
    EXPECT_GT(r.miss_distance_m, 0.0);
    EXPECT_LT(r.miss_distance_m, 1e30);
  }
}

}  // namespace
}  // namespace seashield::sim
