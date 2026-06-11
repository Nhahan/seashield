#include "sim/target.h"

#include <algorithm>

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

TEST(TargetTest, AsmDisabledKeepsLegacyLevelFlight) {
  TargetParams params;  // ASM triggers default to 0 = off.
  params.initial_position = {0, 5000, 10};
  params.heading_rad = math::deg_to_rad(180);
  params.speed_mps = 290;
  Target target(params);
  for (int i = 0; i < 600; ++i) {
    target.step(kDt);
    EXPECT_EQ(target.phase(), Target::Phase::kCruise);
  }
  EXPECT_DOUBLE_EQ(target.position().z, 10.0);  // Sea-skim altitude held exactly.
  EXPECT_DOUBLE_EQ(target.velocity().z, 0.0);
  EXPECT_FALSE(target.weaving());
}

TEST(TargetTest, PopupClimbsThenDivesOntoTheShip) {
  TargetParams params;
  params.initial_position = {0, 6000, 10};  // Sea-skimmer inbound from the north.
  params.heading_rad = math::deg_to_rad(180);
  params.speed_mps = 290;
  params.popup_range_m = 3000.0;
  params.popup_altitude_m = 250.0;
  Target target(params);

  bool saw_popup = false;
  bool saw_terminal = false;
  double max_altitude = 0.0;
  double closest = 1e30;
  for (int i = 0; i < 60 * 40 && !target.destroyed(); ++i) {
    target.step(kDt);
    max_altitude = std::max(max_altitude, target.position().z);
    if (target.phase() == Target::Phase::kPopup) {
      saw_popup = true;
      EXPECT_GT(target.velocity().z, 0.0);  // Climbing.
      EXPECT_NEAR(target.velocity().norm(), params.speed_mps, 1e-9);  // Speed preserved.
    }
    if (target.phase() == Target::Phase::kTerminal) {
      saw_terminal = true;
      EXPECT_LT(target.velocity().z, 0.0);  // Diving.
    }
    const double ground = math::sqrt(target.position().x * target.position().x +
                                     target.position().y * target.position().y);
    closest = std::min(closest, ground + target.position().z);
    if (target.position().z < 1.0 && target.phase() == Target::Phase::kTerminal) {
      break;  // Splashed onto the deck region.
    }
  }
  EXPECT_TRUE(saw_popup);
  EXPECT_TRUE(saw_terminal);
  EXPECT_GE(max_altitude, params.popup_altitude_m * 0.95);
  EXPECT_LT(closest, 300.0) << "terminal dive never converged on the ship";
}

TEST(TargetTest, WeaveOscillatesHeadingAroundBaseCourse) {
  TargetParams params;
  params.initial_position = {0, 5000, 15};
  params.heading_rad = math::deg_to_rad(180);
  params.speed_mps = 250;
  params.weave_range_m = 6000.0;  // Active from the first tick.
  params.weave_turn_rate_rad_s = math::deg_to_rad(15.0);
  params.weave_period_s = 4.0;
  Target target(params);

  double min_heading = 1e9, max_heading = -1e9;
  for (int i = 0; i < 60 * 8; ++i) {  // Two full weave periods.
    target.step(kDt);
    min_heading = std::min(min_heading, target.heading_rad());
    max_heading = std::max(max_heading, target.heading_rad());
  }
  EXPECT_TRUE(target.weaving());
  // The sine overlay must swing the heading both ways around the base course.
  EXPECT_LT(min_heading, math::deg_to_rad(180) - math::deg_to_rad(2));
  EXPECT_GT(max_heading, math::deg_to_rad(180) + math::deg_to_rad(2));
  // Zero-mean weave: net course stays roughly the base heading.
  EXPECT_NEAR(target.heading_rad(), math::deg_to_rad(180), math::deg_to_rad(10));
}

TEST(TargetTest, ManeuversAreBitDeterministicAcrossRuns) {
  const auto run = [] {
    TargetParams params;
    params.initial_position = {2000, 8000, 10};
    params.heading_rad = math::deg_to_rad(195);
    params.speed_mps = 290;
    params.popup_range_m = 3500.0;
    params.weave_range_m = 5000.0;
    Target target(params);
    for (int i = 0; i < 60 * 25; ++i) {
      target.step(kDt);
    }
    return target;
  };
  const Target a = run();
  const Target b = run();
  EXPECT_EQ(a.position().x, b.position().x);  // Bit-identical, not just near.
  EXPECT_EQ(a.position().y, b.position().y);
  EXPECT_EQ(a.position().z, b.position().z);
  EXPECT_EQ(a.heading_rad(), b.heading_rad());
  EXPECT_EQ(a.phase(), b.phase());
  EXPECT_EQ(a.weave_elapsed_s(), b.weave_elapsed_s());
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
