#pragma once

#include "core/math.h"

namespace seashield::sim {

inline constexpr double kTickRateHz = 60.0;
inline constexpr double kTickDt = 1.0 / kTickRateHz;

// Launcher muzzle position (deck height above sea). Shared by the world and
// the fire-control solver — the two MUST agree or every solution is biased
// by the offset.
inline constexpr math::Vec3 kLaunchPosition{0.0, 0.0, 2.0};

}  // namespace seashield::sim
