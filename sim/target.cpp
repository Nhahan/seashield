#include "sim/target.h"

namespace seashield::sim {

double Target::flight_path_angle_rad() const {
  switch (phase_) {
    case Phase::kCruise:
      return 0.0;
    case Phase::kPopup:
      return params_.popup_climb_angle_rad;
    case Phase::kTerminal:
      // Dive at the ship's deck (origin, z = 0): negative path angle that
      // steepens as the missile closes.
      return math::atan2(-position_.z, ground_range_m());
  }
  return 0.0;
}

math::Vec3 Target::velocity() const {
  const double gamma = flight_path_angle_rad();
  const double horizontal = speed_ * math::cos(gamma);
  return {horizontal * math::sin(heading_), horizontal * math::cos(heading_),
          speed_ * math::sin(gamma)};
}

void Target::step(double dt_s) {
  if (destroyed_) {
    return;
  }

  // Triggers run before the kinematic step, so the transition tick already
  // flies the new profile. Ground range to the own ship is the trigger
  // variable for both (charter §5.3 종말 기동).
  const double ground_range = ground_range_m();
  if (!weave_started_ && params_.weave_range_m > 0.0 && ground_range < params_.weave_range_m) {
    weave_started_ = true;  // Sine phase anchors here — deterministic, no RNG.
  }
  if (phase_ == Phase::kCruise && params_.popup_range_m > 0.0 &&
      ground_range < params_.popup_range_m) {
    phase_ = Phase::kPopup;
  }
  if (phase_ == Phase::kPopup && position_.z >= params_.popup_altitude_m) {
    phase_ = Phase::kTerminal;
  }
  if (phase_ == Phase::kTerminal) {
    // Pure pursuit of a static point = straight line, refreshed per tick.
    heading_ = math::atan2(-position_.x, -position_.y);
  }

  // Speed splits into horizontal/vertical by the path angle; the horizontal
  // part feeds the EXACT arc solution of P2 unchanged (γ = 0 makes both
  // factors exact identities, preserving the legacy trajectory bit-for-bit).
  const double gamma = flight_path_angle_rad();
  const double horizontal_speed = speed_ * math::cos(gamma);
  const double vertical_speed = speed_ * math::sin(gamma);

  double turn_rate = turn_rate_;
  if (weave_started_) {
    // Cosine, not sine: the HEADING is the integral of the turn rate, and
    // ∫cos = sin oscillates symmetrically around the base course, whereas
    // ∫sin = 1−cos would push the whole course off to one side. cos(0) = 1
    // means the weave opens at full turn rate — an idealized step-onset
    // evasive break rather than a ramped one.
    turn_rate += params_.weave_turn_rate_rad_s *
                 math::cos(math::kTwoPi * weave_elapsed_s_ / params_.weave_period_s);
    weave_elapsed_s_ += dt_s;
  }

  const double h0 = heading_;
  if (math::sqrt(turn_rate * turn_rate) > 1e-9) {
    const double h1 = h0 + turn_rate * dt_s;
    // Exact integral of speed * (sin h(t), cos h(t)) over the tick.
    position_.x += horizontal_speed * (math::cos(h0) - math::cos(h1)) / turn_rate;
    position_.y += horizontal_speed * (math::sin(h1) - math::sin(h0)) / turn_rate;
    heading_ = h1;
  } else {
    position_.x += horizontal_speed * math::sin(h0) * dt_s;
    position_.y += horizontal_speed * math::cos(h0) * dt_s;
  }
  position_.z += vertical_speed * dt_s;
}

}  // namespace seashield::sim
