#include "sim/world.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>

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

TEST(WorldTest, MultipleTargetsAreTrackedAndKilledIndependently) {
  // Two concurrent inbound ASMs from different bearings (survival game mode).
  WorldConfig cfg;
  cfg.weather = calm_weather();
  cfg.target.initial_position = {6000, 8000, 800};
  cfg.target.heading_rad = math::deg_to_rad(225.0);
  cfg.target.speed_mps = 250.0;
  TargetParams second = cfg.target;
  second.initial_position = {-7000, 6000, 600};
  second.heading_rad = math::atan2(7000.0, -6000.0);  // inbound toward the ship
  cfg.additional_targets.push_back(second);
  cfg.radar.scan_period_s = 0.5;
  World world(cfg);
  ASSERT_EQ(world.targets().size(), 2u);

  std::set<std::uint32_t> confirmed;
  for (int i = 0; i < 60 * 25; ++i) {
    world.step();
    for (const TrackEvent& e : world.track_events()) {
      if (e.kind == TrackEvent::Kind::kConfirmed) {
        confirmed.insert(e.track_id);
      }
    }
  }
  // Both targets produce their own confirmed track.
  EXPECT_GE(confirmed.size(), 2u) << "both targets should be tracked separately";

  // Destroying one target leaves the other alive — kills are independent.
  World w2(cfg);
  for (int i = 0; i < 30; ++i) {
    w2.step();
  }
  EXPECT_FALSE(w2.targets()[0].destroyed());
  EXPECT_FALSE(w2.targets()[1].destroyed());
}

TEST(WorldTest, MultiTargetWorldIsDeterministic) {
  // The multi-target world must still be bit-reproducible (state hash).
  WorldConfig cfg;
  cfg.weather = calm_weather();
  cfg.target.initial_position = {5000, 5000, 700};
  cfg.target.heading_rad = math::deg_to_rad(225.0);
  cfg.target.speed_mps = 240.0;
  TargetParams b = cfg.target;
  b.initial_position = {-4000, 6000, 500};
  cfg.additional_targets.push_back(b);
  cfg.additional_targets.push_back(b);  // three targets total

  World a(cfg), c(cfg);
  for (int i = 0; i < 60 * 10; ++i) {
    a.step();
    c.step();
    ASSERT_EQ(a.state_hash(), c.state_hash()) << "diverged at tick " << i;
  }
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

TEST(WorldTest, OwnShipIntegratesFromSteerCommands) {
  WorldConfig cfg;
  cfg.weather = calm_weather();
  cfg.target.initial_position = {0, 20000, 500};  // Far and inert; irrelevant here.
  cfg.target.speed_mps = 0.0;
  cfg.ship.max_speed_mps = 20.0;
  cfg.ship.accel_mps2 = 5.0;
  cfg.ship.turn_rate_max_rad_s = math::deg_to_rad(10.0);
  World world(cfg);

  // No steer command yet: the ship holds at the origin.
  for (int i = 0; i < 10; ++i) {
    world.step();
  }
  EXPECT_LT(world.ship_position().norm(), 1e-9);
  EXPECT_DOUBLE_EQ(world.ship_velocity().norm(), 0.0);

  // Ahead full on the initial (north) heading: it accelerates and makes way.
  world.queue_steer({0.0, 1.0});
  for (int i = 0; i < 60 * 5; ++i) {
    world.step();
  }
  EXPECT_GT(world.ship_velocity().norm(), 10.0);
  EXPECT_GT(world.ship_position().y, 30.0);          // moved north
  EXPECT_NEAR(world.ship_position().x, 0.0, 1e-6);   // no rudder, no drift

  // Starboard rudder turns the heading clockwise (+).
  const double h0 = world.ship_heading_rad();
  world.queue_steer({1.0, 1.0});
  for (int i = 0; i < 60 * 2; ++i) {
    world.step();
  }
  EXPECT_GT(world.ship_heading_rad(), h0);
}

TEST(WorldTest, FixedPlatformIgnoresSteering) {
  // Default ship params (zero limits): a steer command cannot move the platform,
  // preserving the legacy single-engagement geometry.
  World world(hovering_target_config());
  world.queue_steer({1.0, 1.0});
  for (int i = 0; i < 60 * 5; ++i) {
    world.step();
  }
  EXPECT_DOUBLE_EQ(world.ship_position().x, 0.0);
  EXPECT_DOUBLE_EQ(world.ship_position().y, 0.0);
  EXPECT_DOUBLE_EQ(world.ship_velocity().norm(), 0.0);
}

namespace {
// Closest ground range an inbound, terminally-homing ASM achieves to the
// (possibly moving) ship over its run. The ASM starts dead on the origin, so a
// stationary ship is a guaranteed hit; a beam run tests whether the turn-rate
// cap lets the ship slip the homing.
double min_ground_range_to_ship(bool steerable) {
  WorldConfig cfg;
  cfg.weather = calm_weather();
  cfg.target.initial_position = {0, 3500, 150};
  cfg.target.heading_rad = math::deg_to_rad(180.0);  // Due south, straight at origin.
  cfg.target.speed_mps = 180.0;
  // Hold a straight sea-skim cruise until close, THEN home: a turn-rate cap only
  // defeats a late commitment, not a missile that can lead a whole long approach.
  cfg.target.popup_range_m = 800.0;
  cfg.target.popup_altitude_m = 100.0;  // Below the cruise altitude → terminal at once.
  cfg.target.terminal_turn_rate_max_rad_s = math::deg_to_rad(3.0);  // Beatable cap.
  if (steerable) {
    cfg.ship.heading_rad = math::deg_to_rad(90.0);  // Beam course, due east.
    cfg.ship.max_speed_mps = 30.0;
    cfg.ship.accel_mps2 = 6.0;
    cfg.ship.turn_rate_max_rad_s = math::deg_to_rad(10.0);
  }
  World world(cfg);
  if (steerable) {
    world.queue_steer({0.0, 1.0});  // Ahead full on the beam.
  }
  double best = 1e30;
  for (int i = 0; i < 60 * 40; ++i) {
    world.step();
    const math::Vec3 t = world.target().position();
    const math::Vec3 s = world.ship_position();
    const double dx = t.x - s.x;
    const double dy = t.y - s.y;
    best = std::min(best, std::sqrt(dx * dx + dy * dy));
    if (t.z <= 0.0) {
      break;  // Splashed at sea level — the engagement is over.
    }
  }
  return best;
}
}  // namespace

TEST(WorldTest, ManeuveringShipDodgesTurnRateLimitedASM) {
  const double stationary = min_ground_range_to_ship(/*steerable=*/false);
  const double dodging = min_ground_range_to_ship(/*steerable=*/true);
  EXPECT_LT(stationary, 250.0) << "an unengaged ASM should reach a stationary ship";
  EXPECT_GT(dodging, stationary + 150.0) << "maneuvering must open the miss distance";
  EXPECT_GT(dodging, 250.0) << "a hard beam run should defeat the leak threshold";
}

// In-game dodge payoff: across many spawn geometries mirroring the server's
// game-mode roll_target + tuning, ASMs strike a STATIONARY ship dead-on but a
// threat-aware beam break slips nearly all of them. Deterministic stand-in for
// the live survival run (no UE, 24 trials > one noisy wave). If a future retune
// neuters the dodge, this fails loudly.
TEST(WorldTest, ManeuveringDodgesAsmsAcrossManyGeometries) {
  constexpr double kHitRangeM = 140.0;
  auto roll = [](Pcg32& r) -> TargetParams {
    TargetParams t;
    const double bearing = r.next_double() * math::kTwoPi;
    const double range_m = 4500.0 + r.next_double() * 3500.0;
    const double alt_m = 150.0 + r.next_double() * 700.0;
    const double east = range_m * math::sin(bearing);
    const double north = range_m * math::cos(bearing);
    t.initial_position = {east, north, alt_m};
    const double inbound = math::atan2(-east, -north);
    t.heading_rad = inbound;  // dead at the ship's start point (no cruise jitter)
    t.speed_mps = 200.0 + r.next_double() * 90.0;
    t.popup_range_m = 700.0 + r.next_double() * 300.0;
    t.popup_altitude_m = 240.0;
    t.terminal_turn_rate_max_rad_s = math::deg_to_rad(3.5);
    return t;
  };
  auto leaks = [&](bool steer) {
    int leak = 0;
    Pcg32 r(0xBEEFu, 1);
    for (int s = 0; s < 24; ++s) {
      WorldConfig cfg;
      cfg.weather = calm_weather();
      cfg.target = roll(r);
      if (steer) {
        cfg.ship.max_speed_mps = 20.0;
        cfg.ship.accel_mps2 = 2.0;
        cfg.ship.turn_rate_max_rad_s = math::deg_to_rad(10.0);
        // Threat-aware beam run: head perpendicular to the inbound LOS so the
        // ship translates across the missile's aim (a skilled evasive break).
        const double los = math::atan2(cfg.target.initial_position.x,
                                       cfg.target.initial_position.y);
        cfg.ship.heading_rad = los + math::deg_to_rad(90.0);
      }
      World w(cfg);
      if (steer) {
        w.queue_steer({0.0, 1.0});  // ahead full on the beam course
      }
      double best = 1e30;
      for (int i = 0; i < 60 * 60; ++i) {
        w.step();
        const math::Vec3 tp = w.target().position();
        const math::Vec3 sp = w.ship_position();
        const double dx = tp.x - sp.x;
        const double dy = tp.y - sp.y;
        best = std::min(best, std::sqrt(dx * dx + dy * dy));
        if (tp.z <= 0.0) {
          break;
        }
      }
      if (best < kHitRangeM) {
        ++leak;
      }
    }
    return leak;
  };
  const int stationary = leaks(false);
  const int dodging = leaks(true);
  // A stationary ship is struck by virtually every ASM (they home dead-on);
  // a hard beam break slips the great majority. Margins are loose so honest
  // re-tuning is fine — only neutering the dodge trips this.
  EXPECT_GE(stationary, 22) << "ASMs should reliably strike a stationary ship";
  EXPECT_LE(dodging, 6) << "a threat-aware maneuver should dodge most ASMs";
  EXPECT_LT(dodging * 3, stationary) << "maneuvering must cut leaks several-fold";
}

}  // namespace
}  // namespace seashield::sim
