#pragma once

#include <optional>

#include "sim/ballistics.h"
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

  std::optional<FiringSolution> solve(const math::Vec3& target_position,
                                      const math::Vec3& target_velocity) const;

 private:
  struct Probe {
    math::Vec3 closest_point;
    double time_s = 0.0;
    double miss_m = 0.0;
  };
  // Integrates one trajectory (mean wind, no gust/dispersion) and returns its
  // closest approach to a static aim point.
  Probe shoot(double azimuth_rad, double elevation_rad, const math::Vec3& aim) const;

  Weather weather_;
  Atmosphere atmosphere_;
  WindField wind_;
  RocketParams rocket_;
};

}  // namespace seashield::sim
