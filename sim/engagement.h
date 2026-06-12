#pragma once

#include <algorithm>
#include <cstdint>

#include "core/math.h"

// Per-rocket engagement adjudication (charter §5.7): closest point of
// approach within a tick assuming linear relative motion, proximity fuze,
// and a miss-distance-based kill probability curve.
namespace seashield::sim {

struct ClosestApproach {
  double time_offset_s = 0.0;  // Within [0, dt].
  double distance_m = 0.0;
};

// Minimum distance between two linearly moving points over one tick.
// rel_pos is (rocket - target) at tick start, rel_vel its relative velocity.
inline ClosestApproach closest_approach(const math::Vec3& rel_pos, const math::Vec3& rel_vel,
                                        double dt_s) {
  const double v_sq = rel_vel.norm_squared();
  double t = 0.0;
  if (v_sq > 1e-12) {
    t = std::clamp(-rel_pos.dot(rel_vel) / v_sq, 0.0, dt_s);
  }
  return {t, (rel_pos + rel_vel * t).norm()};
}

// Kill probability from miss distance: 0.95 at a direct hit, falling
// quadratically to 0 at the fuze radius (arbitrary tutorial-grade curve —
// the exact shape is a tunable, documented parameter).
inline double pk_from_miss(double miss_m, double fuze_radius_m) {
  if (miss_m >= fuze_radius_m || fuze_radius_m <= 0.0) {
    return 0.0;
  }
  const double x = miss_m / fuze_radius_m;
  return 0.95 * (1.0 - x * x);
}

struct RocketResult {
  std::uint32_t rocket_id = 0;
  double miss_distance_m = 0.0;  // Best (smallest) over the whole flight.
  bool detonated = false;        // Came within fuze radius.
  bool killed = false;           // Pk roll succeeded AND the target was alive.
  // Pk roll outcome regardless of the one-kill-per-engagement cap: the fuze
  // does not know the target died, so every passage rolls. This is the
  // uncapped per-rocket kill metric the experiment report's independence
  // analysis needs (보고서 §5).
  bool would_kill = false;
  std::uint64_t end_tick = 0;
};

}  // namespace seashield::sim
