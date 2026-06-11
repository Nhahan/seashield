#include "core/matrix.h"

#include <gtest/gtest.h>

namespace seashield::math {
namespace {

TEST(MatrixTest, MultiplyMatchesHandComputedResult) {
  Mat<2, 3> a;
  a(0, 0) = 1.0;
  a(0, 1) = 2.0;
  a(0, 2) = 3.0;
  a(1, 0) = 4.0;
  a(1, 1) = 5.0;
  a(1, 2) = 6.0;
  Mat<3, 2> b;
  b(0, 0) = 7.0;
  b(0, 1) = 8.0;
  b(1, 0) = 9.0;
  b(1, 1) = 10.0;
  b(2, 0) = 11.0;
  b(2, 1) = 12.0;

  const Mat<2, 2> p = a * b;
  EXPECT_DOUBLE_EQ(p(0, 0), 58.0);
  EXPECT_DOUBLE_EQ(p(0, 1), 64.0);
  EXPECT_DOUBLE_EQ(p(1, 0), 139.0);
  EXPECT_DOUBLE_EQ(p(1, 1), 154.0);
}

TEST(MatrixTest, IdentityIsMultiplicativeNeutral) {
  Mat3 a;
  double v = 1.0;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      a(r, c) = v++;
    }
  }
  const Mat3 left = Mat3::identity() * a;
  const Mat3 right = a * Mat3::identity();
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      EXPECT_DOUBLE_EQ(left(r, c), a(r, c));
      EXPECT_DOUBLE_EQ(right(r, c), a(r, c));
    }
  }
}

TEST(MatrixTest, TransposeRoundTripsAndSwapsIndices) {
  Mat<2, 3> a;
  a(0, 2) = 5.0;
  a(1, 0) = -3.0;
  const Mat<3, 2> t = a.transposed();
  EXPECT_DOUBLE_EQ(t(2, 0), 5.0);
  EXPECT_DOUBLE_EQ(t(0, 1), -3.0);
  const Mat<2, 3> back = t.transposed();
  for (int r = 0; r < 2; ++r) {
    for (int c = 0; c < 3; ++c) {
      EXPECT_DOUBLE_EQ(back(r, c), a(r, c));
    }
  }
}

TEST(MatrixTest, ArithmeticAndTrace) {
  Mat3 a = Mat3::identity() * 2.0;
  Mat3 b = Mat3::identity();
  const Mat3 sum = a + b;
  const Mat3 diff = a - b;
  EXPECT_DOUBLE_EQ(sum.trace(), 9.0);
  EXPECT_DOUBLE_EQ(diff.trace(), 3.0);
}

TEST(MatrixTest, ColumnVectorIndexing) {
  Vec6 v;
  v[3] = 42.0;
  EXPECT_DOUBLE_EQ(v(3, 0), 42.0);
  const Vec6 doubled = v * 2.0;
  EXPECT_DOUBLE_EQ(doubled[3], 84.0);
}

TEST(MatrixTest, SpdInverseMatchesAnalyticSolution) {
  // SPD matrix with known inverse: A = [[4,1,0],[1,3,1],[0,1,2]].
  Mat3 a;
  a(0, 0) = 4.0;
  a(0, 1) = 1.0;
  a(1, 0) = 1.0;
  a(1, 1) = 3.0;
  a(1, 2) = 1.0;
  a(2, 1) = 1.0;
  a(2, 2) = 2.0;

  const auto inv = inverse_spd(a);
  ASSERT_TRUE(inv.has_value());
  const Mat3 product = a * *inv;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      EXPECT_NEAR(product(r, c), r == c ? 1.0 : 0.0, 1e-12);
    }
  }
  // Inverse of an SPD matrix is symmetric.
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      EXPECT_NEAR((*inv)(r, c), (*inv)(c, r), 1e-15);
    }
  }
}

TEST(MatrixTest, SingularAndIndefiniteMatricesAreRejected) {
  Mat3 singular;  // Rank 1: all rows identical.
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      singular(r, c) = 1.0;
    }
  }
  EXPECT_FALSE(inverse_spd(singular).has_value());

  // Negative definite: determinant < 0 fails the SPD guard too.
  const Mat3 negative = Mat3::identity() * -1.0;
  EXPECT_FALSE(inverse_spd(negative).has_value());
}

TEST(MatrixTest, KalmanShapedProductCompiles) {
  // The exact shapes the filter uses: K = P Hᵀ S⁻¹ with P 6x6, H 3x6.
  const Mat6 p = Mat6::identity() * 2.0;
  Mat<3, 6> h;
  h(0, 0) = 1.0;
  h(1, 1) = 1.0;
  h(2, 2) = 1.0;
  const Mat3 s = (h * p * h.transposed()) + Mat3::identity();
  const auto s_inv = inverse_spd(s);
  ASSERT_TRUE(s_inv.has_value());
  const Mat<6, 3> k = p * h.transposed() * *s_inv;
  EXPECT_NEAR(k(0, 0), 2.0 / 3.0, 1e-12);  // 2 / (2 + 1).
  EXPECT_NEAR(k(5, 2), 0.0, 1e-12);
}

}  // namespace
}  // namespace seashield::math
