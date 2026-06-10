#include "sim/engagement.h"

#include <gtest/gtest.h>

namespace seashield::sim {
namespace {

TEST(ClosestApproachTest, PerpendicularPass) {
  const auto cpa = closest_approach({-50, 30, 0}, {100, 0, 0}, 1.0);
  EXPECT_NEAR(cpa.time_offset_s, 0.5, 1e-12);
  EXPECT_NEAR(cpa.distance_m, 30.0, 1e-12);
}

TEST(ClosestApproachTest, SeparatingClampsToTickStart) {
  const auto cpa = closest_approach({10, 0, 0}, {5, 0, 0}, 1.0);
  EXPECT_DOUBLE_EQ(cpa.time_offset_s, 0.0);
  EXPECT_DOUBLE_EQ(cpa.distance_m, 10.0);
}

TEST(ClosestApproachTest, ApproachBeyondTickClampsToTickEnd) {
  const auto cpa = closest_approach({-100, 0, 0}, {10, 0, 0}, 1.0);
  EXPECT_DOUBLE_EQ(cpa.time_offset_s, 1.0);
  EXPECT_DOUBLE_EQ(cpa.distance_m, 90.0);
}

TEST(ClosestApproachTest, ZeroRelativeVelocity) {
  const auto cpa = closest_approach({3, 4, 0}, {0, 0, 0}, 1.0);
  EXPECT_DOUBLE_EQ(cpa.time_offset_s, 0.0);
  EXPECT_DOUBLE_EQ(cpa.distance_m, 5.0);
}

TEST(PkCurveTest, ShapeAndBounds) {
  constexpr double kFuze = 12.0;
  EXPECT_DOUBLE_EQ(pk_from_miss(0.0, kFuze), 0.95);
  EXPECT_DOUBLE_EQ(pk_from_miss(kFuze, kFuze), 0.0);
  EXPECT_DOUBLE_EQ(pk_from_miss(kFuze * 2, kFuze), 0.0);
  // Monotonically decreasing inside the fuze radius.
  double prev = 1.0;
  for (double miss = 0.0; miss <= kFuze; miss += 1.0) {
    const double pk = pk_from_miss(miss, kFuze);
    EXPECT_LT(pk, prev);
    EXPECT_GE(pk, 0.0);
    prev = pk;
  }
}

}  // namespace
}  // namespace seashield::sim
