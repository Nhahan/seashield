#include "sim/fire_control.h"

#include <algorithm>

#include "sim/engagement.h"
#include "sim/world.h"

namespace seashield::sim {

namespace {
constexpr math::Vec3 kLaunchPosition{0.0, 0.0, 2.0};
constexpr double kMinElevation = math::deg_to_rad(0.5);
constexpr double kMaxElevation = math::deg_to_rad(80.0);
constexpr double kConvergedMissM = 1.0;
// PIP time fixed-point tolerance. The tolerance leaks into the miss distance
// scaled by the closing speed (~500 m/s), so 5 ms keeps it under ~3 m.
constexpr double kConvergedTimeS = 0.005;
constexpr int kMaxOuterIterations = 20;    // PIP time fixed-point.
constexpr int kMaxNewtonIterations = 15;   // (az, el) shooting per aim point.
}  // namespace

FireControlSolver::FireControlSolver(const Weather& weather, const RocketParams& rocket)
    : weather_(weather),
      atmosphere_(weather),
      wind_(weather.wind_layers),
      rocket_(rocket) {}

FireControlSolver::Probe FireControlSolver::shoot(double azimuth_rad, double elevation_rad,
                                                  const math::Vec3& aim) const {
  FlightEnvironment env;
  env.atmosphere = &atmosphere_;
  env.wind = &wind_;
  env.gravity_mps2 = weather_.gravity_mps2;
  env.rain_drag_multiplier = 1.0 + 0.02 * weather_.rain_intensity;
  // env.gust stays zero: the solver can only compensate the mean wind.

  const math::Vec3 dir = math::direction_from_az_el(azimuth_rad, elevation_rad);
  RocketState s;
  s.position = kLaunchPosition;
  s.velocity = dir * rocket_.rail_exit_speed_mps;

  Probe best;
  best.miss_m = (s.position - aim).norm();
  best.closest_point = s.position;
  while (s.age_s < rocket_.max_lifetime_s && (s.position.z > 0.0 || s.age_s < 0.5)) {
    const math::Vec3 before = s.position;
    rocket_step(s, rocket_, env, dir, kTickDt);
    // Continuous closest approach within the segment — sampling only at tick
    // points would floor the achievable convergence at ~half a tick of travel.
    const math::Vec3 segment_vel = (s.position - before) * kTickRateHz;
    const ClosestApproach cpa = closest_approach(before - aim, segment_vel, kTickDt);
    if (cpa.distance_m < best.miss_m) {
      best.miss_m = cpa.distance_m;
      best.closest_point = before + segment_vel * cpa.time_offset_s;
      best.time_s = (s.age_s - kTickDt) + cpa.time_offset_s;
    } else if ((s.position - aim).norm() > best.miss_m * 2.0 &&
               (s.position - aim).norm() > 200.0) {
      break;  // Receding from the aim point; nothing better will come.
    }
  }
  return best;
}

std::optional<FiringSolution> FireControlSolver::solve(const math::Vec3& target_position,
                                                       const math::Vec3& target_velocity) const {
  // Initial time-of-flight guess from an average ballistic speed.
  double time_of_flight = target_position.norm() / 450.0;
  double az = 0.0;
  double el = 0.0;
  int probes = 0;

  Probe last{};
  bool converged = false;
  for (int outer = 0; outer < kMaxOuterIterations; ++outer) {
    // Predicted intercept point: constant-velocity target extrapolation.
    const math::Vec3 aim = target_position + target_velocity * time_of_flight;

    // Re-initialize on the DIRECT (minimum-time) arc every outer iteration.
    // Warm-starting across iterations can creep onto the lofted branch, whose
    // long flight time blows up the prediction lead and makes the PIP
    // fixed-point oscillate instead of converge.
    az = math::atan2(aim.x, aim.y);
    const double ground_range = math::sqrt(aim.x * aim.x + aim.y * aim.y);
    el = std::clamp(math::atan2(aim.z - kLaunchPosition.z, ground_range) +
                        math::deg_to_rad(8.0),
                    kMinElevation, math::deg_to_rad(45.0));

    // 2D Newton (finite-difference Jacobian) on the miss components
    // perpendicular to the line of sight: crosswind axis and vertical axis.
    const double bearing = math::atan2(aim.x, aim.y);
    const math::Vec3 cross_axis{math::cos(bearing), -math::sin(bearing), 0.0};
    const math::Vec3 up_axis{0.0, 0.0, 1.0};

    for (int iter = 0; iter < kMaxNewtonIterations; ++iter) {
      last = shoot(az, el, aim);
      ++probes;
      if (last.miss_m < kConvergedMissM) {
        break;
      }
      const math::Vec3 miss_vec = last.closest_point - aim;
      const double f0 = miss_vec.dot(cross_axis);
      const double g0 = miss_vec.dot(up_axis);

      constexpr double kDelta = 1e-3;  // rad
      const Probe paz = shoot(az + kDelta, el, aim);
      const Probe pel = shoot(az, el + kDelta, aim);
      probes += 2;
      const math::Vec3 maz = paz.closest_point - aim;
      const math::Vec3 mel = pel.closest_point - aim;
      const double j11 = (maz.dot(cross_axis) - f0) / kDelta;
      const double j12 = (mel.dot(cross_axis) - f0) / kDelta;
      const double j21 = (maz.dot(up_axis) - g0) / kDelta;
      const double j22 = (mel.dot(up_axis) - g0) / kDelta;
      const double det = j11 * j22 - j12 * j21;
      if (math::sqrt(det * det) < 1e-9) {
        break;  // Singular Jacobian: give up on this aim point.
      }
      double daz = (-f0 * j22 + g0 * j12) / det;
      double del = (f0 * j21 - g0 * j11) / det;
      // Damp large steps to keep the shooting stable.
      constexpr double kMaxStep = math::deg_to_rad(10.0);
      daz = std::clamp(daz, -kMaxStep, kMaxStep);
      del = std::clamp(del, -kMaxStep, kMaxStep);
      az += daz;
      el = std::clamp(el + del, kMinElevation, kMaxElevation);
    }

    if (last.time_s <= 0.0) {
      return std::nullopt;
    }
    // Damped fixed-point on the flight time. The aim point of a closing
    // target moves with the assumed time, so the undamped update can
    // oscillate; damping by half converges for |dτ/dt| < 3.
    const double residual = last.time_s - time_of_flight;
    if (math::sqrt(residual * residual) < kConvergedTimeS &&
        last.miss_m < kConvergedMissM) {
      converged = true;
      break;
    }
    time_of_flight += 0.5 * residual;
    if (time_of_flight <= 0.2) {
      return std::nullopt;
    }
  }

  // A solution that did not converge is not a solution — returning the last
  // probe would hand the operator a self-inconsistent firing angle.
  if (!converged || last.miss_m > 50.0) {
    return std::nullopt;
  }
  FiringSolution solution;
  solution.azimuth_rad = az;
  solution.elevation_rad = el;
  solution.time_of_flight_s = last.time_s;
  solution.predicted_miss_m = last.miss_m;
  solution.probe_count = probes;
  return solution;
}

}  // namespace seashield::sim
