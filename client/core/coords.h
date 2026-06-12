#pragma once

#include <numbers>

// Frame mapping between the simulation and the UE5 client (charter §7).
//
// Sim frame: ENU meters — x east, y north, z up, right-handed. Azimuth is
// radians from north (+y), clockwise toward east (target.cpp velocity()).
// UE frame: centimeters — X forward, Y right, Z up, LEFT-handed; yaw is
// degrees about Z, positive clockwise seen from above.
//
// Mapping north onto UE +X makes the handedness flip exactly the x/y swap,
// and sim azimuth becomes UE yaw with only a unit change. Pure functions on
// doubles: the UE module wraps them into FVector/FRotator at the boundary,
// and the tests pin the conventions headlessly.
namespace seashield::client {

inline constexpr double kMetersToCm = 100.0;

struct UeVector {
  double x = 0.0;  // Forward (sim north).
  double y = 0.0;  // Right (sim east).
  double z = 0.0;  // Up.
};

constexpr UeVector to_ue_cm(double east_m, double north_m, double up_m) {
  return {north_m * kMetersToCm, east_m * kMetersToCm, up_m * kMetersToCm};
}

struct EnuVector {
  double east_m = 0.0;
  double north_m = 0.0;
  double up_m = 0.0;
};

constexpr EnuVector to_enu_m(const UeVector& ue) {
  return {ue.y / kMetersToCm, ue.x / kMetersToCm, ue.z / kMetersToCm};
}

// Velocities share the axis mapping; only the scale differs from positions.
constexpr UeVector velocity_to_ue_cms(double east_mps, double north_mps, double up_mps) {
  return to_ue_cm(east_mps, north_mps, up_mps);
}

constexpr double azimuth_rad_to_ue_yaw_deg(double azimuth_rad) {
  return azimuth_rad * 180.0 / std::numbers::pi;
}

constexpr double ue_yaw_deg_to_azimuth_rad(double yaw_deg) {
  return yaw_deg * std::numbers::pi / 180.0;
}

}  // namespace seashield::client
