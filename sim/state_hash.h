#pragma once

#include <bit>
#include <cstdint>

#include "core/math.h"

namespace seashield::sim {

// FNV-1a accumulator over the exact bit patterns of the simulation state.
// Two runs are "deterministically identical" iff their hash sequences match —
// the foundation of the determinism regression tests (charter §5.1/§10.2).
class StateHasher {
 public:
  void mix(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h_ ^= (v >> (i * 8)) & 0xFFu;
      h_ *= 1099511628211ULL;
    }
  }
  void mix(double v) { mix(std::bit_cast<std::uint64_t>(v)); }
  void mix(bool v) { mix(static_cast<std::uint64_t>(v)); }
  void mix(const math::Vec3& v) {
    mix(v.x);
    mix(v.y);
    mix(v.z);
  }

  std::uint64_t value() const { return h_; }

 private:
  std::uint64_t h_ = 1469598103934665603ULL;
};

}  // namespace seashield::sim
