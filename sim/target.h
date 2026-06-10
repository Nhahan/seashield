#pragma once

#include "core/math.h"

// Air target: 3DOF point-mass in level flight with an optional constant-rate
// turn (charter §5.3). The turning update is the exact circular-arc solution,
// not an Euler step, so trajectories are accurate and deterministic.
namespace seashield::sim {

struct TargetParams {
  math::Vec3 initial_position{8000.0, 8000.0, 300.0};
  double heading_rad = math::deg_to_rad(225.0);  // Direction of travel (clockwise from North).
  double speed_mps = 250.0;
  double turn_rate_rad_s = 0.0;  // Positive = clockwise.
};

class Target {
 public:
  Target() = default;
  explicit Target(const TargetParams& params)
      : position_(params.initial_position),
        heading_(params.heading_rad),
        speed_(params.speed_mps),
        turn_rate_(params.turn_rate_rad_s) {}

  void step(double dt_s) {
    if (destroyed_) {
      return;
    }
    const double h0 = heading_;
    if (math::sqrt(turn_rate_ * turn_rate_) > 1e-9) {
      const double h1 = h0 + turn_rate_ * dt_s;
      // Exact integral of speed * (sin h(t), cos h(t)) over the tick.
      position_.x += speed_ * (math::cos(h0) - math::cos(h1)) / turn_rate_;
      position_.y += speed_ * (math::sin(h1) - math::sin(h0)) / turn_rate_;
      heading_ = h1;
    } else {
      position_.x += speed_ * math::sin(h0) * dt_s;
      position_.y += speed_ * math::cos(h0) * dt_s;
    }
  }

  const math::Vec3& position() const { return position_; }
  math::Vec3 velocity() const {
    return {speed_ * math::sin(heading_), speed_ * math::cos(heading_), 0.0};
  }
  double heading_rad() const { return heading_; }
  bool destroyed() const { return destroyed_; }
  void destroy() { destroyed_ = true; }

 private:
  math::Vec3 position_;
  double heading_ = 0.0;
  double speed_ = 0.0;
  double turn_rate_ = 0.0;
  bool destroyed_ = false;
};

}  // namespace seashield::sim
