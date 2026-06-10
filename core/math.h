#pragma once

#include <cmath>

// Math primitives for the deterministic simulation core.
//
// All transcendental calls in simulation code go through this namespace so
// the implementation can be swapped for a fixed one if cross-binary
// determinism ever becomes a goal (charter §5.1). Coordinates are local ENU:
// x = East, y = North, z = Up (charter §5.2).
namespace seashield::math {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;

using std::atan2;
using std::cos;
using std::exp;
using std::log;
using std::pow;
using std::sin;
using std::sqrt;
using std::tan;

inline constexpr double deg_to_rad(double deg) { return deg * (kPi / 180.0); }
inline constexpr double rad_to_deg(double rad) { return rad * (180.0 / kPi); }
// Dispersion is specified in milliradians.
inline constexpr double mrad_to_rad(double mrad) { return mrad * 1e-3; }

struct Vec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;

  constexpr Vec3() = default;
  constexpr Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

  constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  constexpr Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
  constexpr Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
  constexpr Vec3 operator-() const { return {-x, -y, -z}; }

  Vec3& operator+=(const Vec3& o) {
    x += o.x;
    y += o.y;
    z += o.z;
    return *this;
  }
  Vec3& operator-=(const Vec3& o) {
    x -= o.x;
    y -= o.y;
    z -= o.z;
    return *this;
  }

  constexpr double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
  constexpr Vec3 cross(const Vec3& o) const {
    return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
  }
  constexpr double norm_squared() const { return dot(*this); }
  double norm() const { return sqrt(norm_squared()); }
  Vec3 normalized() const {
    const double n = norm();
    return n > 0.0 ? *this / n : Vec3{};
  }
};

inline constexpr Vec3 operator*(double s, const Vec3& v) { return v * s; }

// Unit vector for azimuth (clockwise from North) and elevation above horizon.
inline Vec3 direction_from_az_el(double azimuth_rad, double elevation_rad) {
  const double cos_el = cos(elevation_rad);
  return {sin(azimuth_rad) * cos_el, cos(azimuth_rad) * cos_el, sin(elevation_rad)};
}

}  // namespace seashield::math
