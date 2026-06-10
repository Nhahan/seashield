#include "sim/target.h"

#include <gtest/gtest.h>

namespace seashield::sim {
namespace {

constexpr double kDt = 1.0 / 60.0;

TEST(TargetTest, StraightFlightAdvancesAlongHeading) {
  TargetParams params;
  params.initial_position = {0, 0, 300};
  params.heading_rad = math::deg_to_rad(90);  // East.
  params.speed_mps = 100;
  params.turn_rate_rad_s = 0;
  Target target(params);
  for (int i = 0; i < 120; ++i) {
    target.step(kDt);
  }
  EXPECT_NEAR(target.position().x, 200.0, 1e-9);
  EXPECT_NEAR(target.position().y, 0.0, 1e-9);
  EXPECT_NEAR(target.position().z, 300.0, 1e-12);  // Level flight.
}

TEST(TargetTest, ConstantTurnFollowsExactCircle) {
  TargetParams params;
  params.initial_position = {0, 0, 500};
  params.heading_rad = 0.0;  // North, turning clockwise (east).
  params.speed_mps = 100;
  params.turn_rate_rad_s = math::deg_to_rad(9);  // Quarter turn in 10 s.
  Target target(params);

  const double radius = params.speed_mps / params.turn_rate_rad_s;
  const math::Vec3 center{radius, 0, 500};
  for (int i = 0; i < 600; ++i) {  // 10 seconds.
    target.step(kDt);
    EXPECT_NEAR((target.position() - center).norm(), radius, 1e-6);
  }
  EXPECT_NEAR(target.heading_rad(), math::deg_to_rad(90), 1e-9);
  EXPECT_NEAR(target.position().x, radius, 1e-6);
  EXPECT_NEAR(target.position().y, radius, 1e-6);
}

TEST(TargetTest, DestroyedTargetStopsMoving) {
  TargetParams params;
  params.speed_mps = 250;
  Target target(params);
  target.destroy();
  const math::Vec3 before = target.position();
  target.step(kDt);
  EXPECT_DOUBLE_EQ(target.position().x, before.x);
  EXPECT_TRUE(target.destroyed());
}

}  // namespace
}  // namespace seashield::sim
