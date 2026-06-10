#include "core/math.h"

#include <gtest/gtest.h>

namespace seashield::math {
namespace {

TEST(Vec3Test, BasicOperations) {
  const Vec3 a{1, 2, 3};
  const Vec3 b{4, 5, 6};
  const Vec3 sum = a + b;
  EXPECT_DOUBLE_EQ(sum.x, 5);
  EXPECT_DOUBLE_EQ(sum.y, 7);
  EXPECT_DOUBLE_EQ(sum.z, 9);
  EXPECT_DOUBLE_EQ(a.dot(b), 32);
  const Vec3 c = a.cross(b);
  EXPECT_DOUBLE_EQ(c.x, -3);
  EXPECT_DOUBLE_EQ(c.y, 6);
  EXPECT_DOUBLE_EQ(c.z, -3);
  EXPECT_DOUBLE_EQ((a * 2.0).y, 4);
  EXPECT_DOUBLE_EQ((2.0 * a).y, 4);
}

TEST(Vec3Test, NormAndNormalized) {
  const Vec3 v{3, 4, 0};
  EXPECT_DOUBLE_EQ(v.norm(), 5);
  EXPECT_DOUBLE_EQ(v.normalized().norm(), 1);
  EXPECT_DOUBLE_EQ(Vec3{}.normalized().norm(), 0);  // Zero vector stays zero.
}

TEST(MathTest, DirectionFromAzEl) {
  // Azimuth 0 = North (+y), elevation 0 = horizontal.
  const Vec3 north = direction_from_az_el(0, 0);
  EXPECT_NEAR(north.x, 0, 1e-12);
  EXPECT_NEAR(north.y, 1, 1e-12);
  EXPECT_NEAR(north.z, 0, 1e-12);
  // Azimuth 90° = East (+x).
  const Vec3 east = direction_from_az_el(deg_to_rad(90), 0);
  EXPECT_NEAR(east.x, 1, 1e-12);
  EXPECT_NEAR(east.y, 0, 1e-12);
  // Elevation 90° = straight up regardless of azimuth.
  const Vec3 up = direction_from_az_el(deg_to_rad(37), deg_to_rad(90));
  EXPECT_NEAR(up.z, 1, 1e-12);
  EXPECT_NEAR(up.norm(), 1, 1e-12);
}

}  // namespace
}  // namespace seashield::math
