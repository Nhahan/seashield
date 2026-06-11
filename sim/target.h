#pragma once

#include <cstdint>

#include "core/math.h"

// Air target: 3DOF point-mass (charter §5.3). The base behaviour is level
// flight with an optional constant-rate turn (the exact circular-arc
// solution, P2). P4 adds the anti-ship-missile threat profile: sea-skimming
// cruise (just a low target_z), a pop-up climb triggered by ground range to
// the own ship, a terminal dive onto the ship, and a weaving overlay.
//
// All maneuvers are DETERMINISTIC — no randomness. Evasion works by being
// unpredictable to the fire-control solver, and a sine weave already is;
// the weave phase is anchored to its trigger instant (charter §5.1).
namespace seashield::sim {

struct TargetParams {
  math::Vec3 initial_position{8000.0, 8000.0, 300.0};
  double heading_rad = math::deg_to_rad(225.0);  // Direction of travel (clockwise from North).
  double speed_mps = 250.0;
  double turn_rate_rad_s = 0.0;  // Positive = clockwise.

  // --- ASM maneuver extension. Range triggers compare ground range to the
  // own ship (origin); 0 disables = the legacy behaviour, bit-for-bit. ---
  double popup_range_m = 0.0;
  double popup_altitude_m = 250.0;
  double popup_climb_angle_rad = math::deg_to_rad(25.0);
  double weave_range_m = 0.0;
  double weave_turn_rate_rad_s = math::deg_to_rad(15.0);  // Oscillation amplitude.
  double weave_period_s = 4.0;
};

class Target {
 public:
  // kCruise: level flight (legacy). kPopup: climb at the fixed path angle
  // until the pop-up altitude. kTerminal: pure pursuit dive onto the ship.
  enum class Phase : std::uint8_t { kCruise = 0, kPopup = 1, kTerminal = 2 };

  Target() = default;
  explicit Target(const TargetParams& params)
      : params_(params),
        position_(params.initial_position),
        heading_(params.heading_rad),
        speed_(params.speed_mps),
        turn_rate_(params.turn_rate_rad_s) {}

  void step(double dt_s);

  const math::Vec3& position() const { return position_; }
  // Full velocity including the vertical component in pop-up/terminal phases
  // — both the radar's elevation measurements and the fire-control
  // extrapolation see this vector.
  math::Vec3 velocity() const;
  double heading_rad() const { return heading_; }
  double speed_mps() const { return speed_; }
  double turn_rate_rad_s() const { return turn_rate_; }
  Phase phase() const { return phase_; }
  bool weaving() const { return weave_started_; }
  double weave_elapsed_s() const { return weave_elapsed_s_; }
  bool destroyed() const { return destroyed_; }
  void destroy() { destroyed_ = true; }

 private:
  double ground_range_m() const {
    return math::sqrt(position_.x * position_.x + position_.y * position_.y);
  }
  // Flight path angle γ by phase: 0 / +climb / dive at the ship's deck.
  double flight_path_angle_rad() const;

  TargetParams params_;
  math::Vec3 position_;
  // Kinematic state mirrors the params so a default-constructed Target stays
  // inert (speed 0) — the pre-P4 semantics tests rely on.
  double heading_ = 0.0;
  double speed_ = 0.0;
  double turn_rate_ = 0.0;
  Phase phase_ = Phase::kCruise;
  bool weave_started_ = false;
  double weave_elapsed_s_ = 0.0;
  bool destroyed_ = false;
};

}  // namespace seashield::sim
