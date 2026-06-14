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

  // Terminal-phase heading slew limit (rad/s). 0 = UNLIMITED: the heading snaps
  // straight at the ship every tick — the legacy instant homing, bit-for-bit.
  // A finite cap models a real ASM's turn limit, so a hard own-ship maneuver
  // can force the missile to overshoot — the dodge mechanic (charter §5.3).
  double terminal_turn_rate_max_rad_s = 0.0;
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

  // ship_pos is the own ship's current position; the pop-up/weave/terminal
  // triggers and the terminal homing aim are all measured relative to it. A
  // fixed platform at the origin reproduces the legacy behaviour bit-for-bit.
  void step(double dt_s, const math::Vec3& ship_pos = math::Vec3{});

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
  // Ground range to the own ship (the aim point). Defaults to the origin until
  // step() latches the ship's pose, so a default-constructed target measures
  // range to the origin exactly as before.
  double ground_range_m() const {
    const double dx = position_.x - ship_ref_.x;
    const double dy = position_.y - ship_ref_.y;
    return math::sqrt(dx * dx + dy * dy);
  }
  // Flight path angle γ by phase: 0 / +climb / dive at the ship's deck.
  double flight_path_angle_rad() const;

  TargetParams params_;
  math::Vec3 position_;
  // The own ship's position this tick — the homing/trigger reference. Latched
  // at the top of step(); origin by default (legacy fixed platform).
  math::Vec3 ship_ref_{0.0, 0.0, 0.0};
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
