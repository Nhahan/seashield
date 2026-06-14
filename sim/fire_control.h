#pragma once

#include <optional>

#include "sim/ballistics.h"
#include "sim/constants.h"
#include "sim/environment.h"

// Truth-based fire-control solution (charter §5.6 항목 3): finds the firing
// angles whose ballistic trajectory meets the constant-velocity extrapolation
// of the target — the predicted intercept point (PIP) — by numeric shooting.
//
// Deliberate limits, which ARE the simulation content:
//  - only the MEAN wind is compensated; gusts are unpredictable by design,
//  - the target is extrapolated at constant velocity; a turning target
//    invalidates the prediction (quantified in the experiments report),
//  - P2 feeds the solver truth state; P4 replaces it with the Kalman track.
namespace seashield::sim {

struct FiringSolution {
  double azimuth_rad = 0.0;
  double elevation_rad = 0.0;
  double time_of_flight_s = 0.0;
  // Residual distance to the predicted point — solver convergence quality,
  // NOT the expected engagement miss (gusts/dispersion/maneuver add to it).
  double predicted_miss_m = 0.0;
  int probe_count = 0;
};

class FireControlSolver {
 public:
  FireControlSolver(const Weather& weather, const RocketParams& rocket);

  // launch_position / launch_velocity describe the launcher's deck and its
  // inherited motion. The defaults (fixed deck at the origin, no inheritance)
  // reproduce the legacy truth-based solution bit-for-bit; a moving own ship
  // passes its launch pose so the solution compensates the platform velocity.
  std::optional<FiringSolution> solve(const math::Vec3& target_position,
                                      const math::Vec3& target_velocity,
                                      const math::Vec3& launch_position = kLaunchPosition,
                                      const math::Vec3& launch_velocity = math::Vec3{}) const;

 private:
  struct Probe {
    math::Vec3 closest_point;
    double time_s = 0.0;
    double miss_m = 0.0;
  };
  // Integrates one trajectory (mean wind, no gust/dispersion) and returns its
  // closest approach to a static aim point, from launch_position with the
  // launcher's inherited launch_velocity added to the muzzle velocity.
  Probe shoot(double azimuth_rad, double elevation_rad, const math::Vec3& aim,
              const math::Vec3& launch_position, const math::Vec3& launch_velocity) const;

  Weather weather_;
  Atmosphere atmosphere_;
  WindField wind_;
  RocketParams rocket_;
};

}  // namespace seashield::sim
