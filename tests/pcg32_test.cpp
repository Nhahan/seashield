#include "core/pcg32.h"

#include <cmath>

#include <gtest/gtest.h>

namespace seashield {
namespace {

TEST(Pcg32Test, SameSeedSameSequence) {
  Pcg32 a(42, 7);
  Pcg32 b(42, 7);
  for (int i = 0; i < 1000; ++i) {
    ASSERT_EQ(a.next(), b.next());
  }
}

TEST(Pcg32Test, DifferentSeedOrStreamDiverges) {
  Pcg32 a(42, 7);
  Pcg32 b(43, 7);
  Pcg32 c(42, 8);
  int diff_seed = 0;
  int diff_stream = 0;
  for (int i = 0; i < 100; ++i) {
    const std::uint32_t va = a.next();
    diff_seed += (va != b.next());
    diff_stream += (va != c.next());
  }
  EXPECT_GT(diff_seed, 90);
  EXPECT_GT(diff_stream, 90);
}

TEST(Pcg32Test, UniformBoundsAndMean) {
  Pcg32 rng(123);
  double sum = 0;
  constexpr int kSamples = 100000;
  for (int i = 0; i < kSamples; ++i) {
    const double v = rng.next_double();
    ASSERT_GE(v, 0.0);
    ASSERT_LT(v, 1.0);
    sum += v;
  }
  EXPECT_NEAR(sum / kSamples, 0.5, 0.01);
}

TEST(Pcg32Test, GaussianMoments) {
  Pcg32 rng(777);
  constexpr int kSamples = 100000;
  constexpr double kMean = 3.0;
  constexpr double kSigma = 2.0;
  double sum = 0;
  double sum_sq = 0;
  for (int i = 0; i < kSamples; ++i) {
    const double v = rng.gaussian(kMean, kSigma);
    sum += v;
    sum_sq += v * v;
  }
  const double mean = sum / kSamples;
  const double var = sum_sq / kSamples - mean * mean;
  EXPECT_NEAR(mean, kMean, 0.05);
  EXPECT_NEAR(std::sqrt(var), kSigma, 0.05);
}

}  // namespace
}  // namespace seashield
