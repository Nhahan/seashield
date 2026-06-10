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
