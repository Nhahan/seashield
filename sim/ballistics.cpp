#include "sim/ballistics.h"

#include <algorithm>

namespace seashield::sim {

math::Vec3 rocket_acceleration(const RocketState& state, const RocketParams& params,
                               const FlightEnvironment& env, const math::Vec3& launch_dir) {
  math::Vec3 accel{0.0, 0.0, -env.gravity_mps2};

  if (state.age_s < params.burn_time_s && params.thrust_n > 0.0) {
    const double speed = state.velocity.norm();
    const math::Vec3 thrust_dir = speed > 1.0 ? state.velocity / speed : launch_dir;
    accel += thrust_dir * (params.thrust_n / params.mass_kg);
  }

  if (env.atmosphere != nullptr && params.cda_m2 > 0.0) {
    const double altitude = std::max(0.0, state.position.z);
    const math::Vec3 wind = env.wind != nullptr ? env.wind->wind_at(altitude) : math::Vec3{};
    const math::Vec3 v_rel = state.velocity - wind - env.gust;
    const double rel_speed = v_rel.norm();
    if (rel_speed > 0.0) {
      const double rho = env.atmosphere->density(altitude);
      const double k =
          0.5 * rho * params.cda_m2 * env.rain_drag_multiplier / params.mass_kg;
      accel -= v_rel * (k * rel_speed);
    }
  }
  return accel;
}

void rocket_step(RocketState& state, const RocketParams& params, const FlightEnvironment& env,
                 const math::Vec3& launch_dir, double dt_s) {
  struct Derivative {
    math::Vec3 dp;
    math::Vec3 dv;
  };
  const auto eval = [&](const math::Vec3& p, const math::Vec3& v, double age) {
    const RocketState probe{p, v, age};
    return Derivative{v, rocket_acceleration(probe, params, env, launch_dir)};
  };

  const double half = dt_s * 0.5;
  const Derivative k1 = eval(state.position, state.velocity, state.age_s);
  const Derivative k2 = eval(state.position + k1.dp * half, state.velocity + k1.dv * half,
                             state.age_s + half);
  const Derivative k3 = eval(state.position + k2.dp * half, state.velocity + k2.dv * half,
                             state.age_s + half);
  const Derivative k4 =
      eval(state.position + k3.dp * dt_s, state.velocity + k3.dv * dt_s, state.age_s + dt_s);

  state.position += (k1.dp + (k2.dp + k3.dp) * 2.0 + k4.dp) * (dt_s / 6.0);
  state.velocity += (k1.dv + (k2.dv + k3.dv) * 2.0 + k4.dv) * (dt_s / 6.0);
  state.age_s += dt_s;
}

}  // namespace seashield::sim
