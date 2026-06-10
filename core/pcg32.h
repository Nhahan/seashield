#pragma once

#include <cstdint>

#include "core/math.h"

namespace seashield {

// PCG32 (oneseq with selectable stream) — small, fast, seeded PRNG.
// Every random draw in the simulation (weather, dispersion, gusts, Pk rolls)
// comes from explicitly seeded instances of this class, which is what makes
// runs bit-reproducible (charter §5.1). Distinct subsystems use distinct
// stream ids so adding draws in one subsystem does not shift another.
class Pcg32 {
 public:
  explicit Pcg32(std::uint64_t seed, std::uint64_t stream = 0) : inc_((stream << 1u) | 1u) {
    next();
    state_ += seed;
    next();
  }

  std::uint32_t next() {
    const std::uint64_t old = state_;
    state_ = old * 6364136223846793005ULL + inc_;
    const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
    const auto rot = static_cast<std::uint32_t>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
  }

  // Uniform in [0, 1).
  double next_double() { return next() * (1.0 / 4294967296.0); }

  double uniform(double lo, double hi) { return lo + (hi - lo) * next_double(); }

  // Box-Muller; always consumes exactly two draws (no cached spare) so the
  // draw count per call is fixed and reproducible.
  double gaussian(double mean, double sigma) {
    const double u1 = (static_cast<double>(next()) + 1.0) * (1.0 / 4294967296.0);  // (0, 1]
    const double u2 = next() * (1.0 / 4294967296.0);
    return mean + sigma * math::sqrt(-2.0 * math::log(u1)) * math::cos(math::kTwoPi * u2);
  }

  // Internal state, exposed so world hashing can capture RNG progress.
  std::uint64_t state() const { return state_; }

 private:
  std::uint64_t state_ = 0;
  std::uint64_t inc_;
};

}  // namespace seashield
