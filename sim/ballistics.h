#pragma once

#include "core/math.h"
#include "sim/environment.h"

// Exterior ballistics for the unguided rocket (charter §5.6): point-mass
// 3DOF, boost-glide thrust profile, quadratic drag against the air-relative
// velocity — which is how wind and gusts push the trajectory.
namespace seashield::sim {

struct RocketParams {
  double mass_kg = 60.0;
  double cda_m2 = 0.012;  // Drag coefficient × reference area.
  double thrust_n = 18000.0;
  double burn_time_s = 1.5;
  double rail_exit_speed_mps = 30.0;
  double max_lifetime_s = 40.0;
  double proximity_fuze_radius_m = 12.0;
};

// Environment as seen by a projectile in flight. The gust vector is sampled
// once per tick and held constant across RK4 substeps (documented
// simplification: the OU process advances at tick rate).
struct FlightEnvironment {
  const Atmosphere* atmosphere = nullptr;  // nullptr = vacuum (tests).
  const WindField* wind = nullptr;         // nullptr = calm.
  math::Vec3 gust;
  double gravity_mps2 = 9.80665;
  double rain_drag_multiplier = 1.0;  // Placeholder effect; main rain use is P4 radar.
};

struct RocketState {
  math::Vec3 position;
  math::Vec3 velocity;
  double age_s = 0.0;
};

// Point-mass acceleration. launch_dir defines the thrust direction while the
// speed is too small for the velocity vector to define one.
math::Vec3 rocket_acceleration(const RocketState& state, const RocketParams& params,
                               const FlightEnvironment& env, const math::Vec3& launch_dir);

// Advances the state by one fixed RK4 step of dt_s seconds.
void rocket_step(RocketState& state, const RocketParams& params, const FlightEnvironment& env,
                 const math::Vec3& launch_dir, double dt_s);

}  // namespace seashield::sim
