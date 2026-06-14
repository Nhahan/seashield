#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/pcg32.h"
#include "sim/ballistics.h"
#include "sim/engagement.h"
#include "sim/environment.h"
#include "sim/fire_control.h"
#include "sim/radar.h"
#include "sim/target.h"
#include "sim/tracking.h"

#include "sim/constants.h"

// Fixed-tick (60Hz) authoritative world: weather, one air target, the sensor
// chain (radar scan -> Kalman tracker, P4), rocket salvos in flight, and
// engagement adjudication. Update order inside step() is fixed and documented
// — part of the determinism contract (charter §5.1).
namespace seashield::sim {

// The only external input of P2: a fire command. Journaled with the tick it
// was queued at (charter §5.8).
struct FireCommand {
  double azimuth_rad = 0.0;
  double elevation_rad = math::deg_to_rad(45.0);
  int salvo_count = 1;
  double dispersion_mrad = 5.0;
  double launch_interval_s = 0.05;
};

// Continuous steering set-points for the own ship (survival game mode, P7+).
// Held until the next command; journaled with the tick like FireCommand, so a
// replay reproduces the maneuver (charter §5.8). The single-engagement /
// golden path never issues one — the ship stays a fixed platform.
struct SteerCommand {
  double rudder = 0.0;    // -1..+1, + = turn to starboard (clockwise from North).
  double throttle = 0.0;  // 0..1, fraction of max speed.
};

// Own-ship platform parameters. The defaults make a FIXED platform at the
// origin — bit-for-bit the legacy world (steering is inert when both limits
// are zero), so only state_hash() notices the added ship fields.
struct ShipParams {
  math::Vec3 initial_position{0.0, 0.0, 0.0};  // Sea-level platform.
  double heading_rad = 0.0;          // Course over ground, CW from North.
  double speed_mps = 0.0;
  double max_speed_mps = 0.0;        // 0 = cannot accelerate (fixed platform).
  double accel_mps2 = 2.0;           // Throttle response toward the set-point.
  double turn_rate_max_rad_s = 0.0;  // 0 = cannot turn (fixed platform).
};

// Integrated own-ship state. The launch point rides the deck and rockets
// inherit the ship velocity. Pure kinematics — no RNG — so adding it leaves
// the N=1 RNG consumption order untouched (charter §5.1).
struct OwnShip {
  math::Vec3 position{0.0, 0.0, 0.0};
  double heading_rad = 0.0;
  double speed_mps = 0.0;
  double rudder = 0.0;    // Current held set-point.
  double throttle = 0.0;  // Current held set-point.
  ShipParams params;

  math::Vec3 velocity() const {
    return {speed_mps * math::sin(heading_rad), speed_mps * math::cos(heading_rad), 0.0};
  }
  // The launcher sits on the deck, above the ship's surface position.
  math::Vec3 launch_position() const {
    return {position.x, position.y, position.z + kLaunchPosition.z};
  }
  void step(double dt_s);
};

struct WorldConfig {
  Weather weather;
  RocketParams rocket;
  TargetParams target;  // The primary target (index 0); the legacy single-target.
  // Extra simultaneous targets (survival game mode). Empty = the deterministic
  // single-target world (replay/experiment/golden) — identical to before.
  std::vector<TargetParams> additional_targets;
  RadarParams radar;
  TrackerParams tracker;
  ShipParams ship;              // Own ship; default = fixed platform at origin.
  std::uint64_t sim_seed = 1;   // Dispersion + Pk + radar streams.
  std::uint64_t gust_seed = 1;  // Gust process stream.
};

struct Rocket {
  std::uint32_t id = 0;
  RocketState state;
  math::Vec3 launch_dir;
  bool alive = true;
  double best_miss_m = 1e30;
};

struct Event {
  std::uint64_t tick = 0;
  std::string text;
};

class World {
 public:
  explicit World(const WorldConfig& config);

  // Queues a command; it takes effect at the start of the next step().
  void queue_fire(const FireCommand& command);
  // Queues a steering set-point; applied at the start of the next step().
  void queue_steer(const SteerCommand& command);

  void step();

  std::uint64_t tick() const { return tick_; }
  double time_s() const { return static_cast<double>(tick_) * kTickDt; }

  const Weather& weather() const { return config_.weather; }
  // Primary target (index 0) — the single-target accessor kept for replay /
  // experiment / tests. targets() exposes all concurrent targets (game mode).
  const Target& target() const { return targets_[0]; }
  const std::vector<Target>& targets() const { return targets_; }
  // Own ship (survival game mode). Default is a fixed platform at the origin,
  // so single-engagement consumers see the legacy geometry.
  const OwnShip& ship() const { return ship_; }
  math::Vec3 ship_position() const { return ship_.position; }
  math::Vec3 ship_velocity() const { return ship_.velocity(); }
  double ship_heading_rad() const { return ship_.heading_rad; }
  const Radar& radar() const { return radar_; }
  const Tracker& tracker() const { return tracker_; }
  const std::vector<Rocket>& rockets() const { return rockets_; }
  const std::vector<RocketResult>& results() const { return results_; }
  const std::vector<Event>& events() const { return events_; }
  // Structured track lifecycle log, append-only and tick-ordered — consumed
  // by the server with the same cursor pattern as rockets()/results().
  const std::vector<TrackEvent>& track_events() const { return track_events_; }

  // True once every queued/launched rocket has been resolved.
  bool ordnance_resolved() const;

  // Fire-control solution for a CONFIRMED track's estimated state — the P4
  // path where sensor error propagates into aiming error (charter §5.6).
  // const and RNG-free by design: live runs may call it any number of times
  // while a replay calls it never, and the state hash must not notice.
  enum class SolveForTrackError : std::uint8_t {
    kNoSuchTrack = 0,
    kNotConfirmed = 1,  // Firing on a tentative track is procedurally refused.
    kNoSolution = 2,    // Solver did not converge (geometry out of reach).
    kStale = 3,         // Confirmed but coasting past TrackerParams::max_coast_scans.
  };
  std::optional<FiringSolution> solve_for_track(std::uint32_t track_id,
                                                SolveForTrackError* reason = nullptr) const;

  // Hash of the full mutable state including RNG progress (charter §10.2).
  std::uint64_t state_hash() const;

 private:
  struct ScheduledLaunch {
    std::uint64_t tick = 0;
    double azimuth_rad = 0.0;
    double elevation_rad = 0.0;
    double dispersion_mrad = 0.0;
  };

  void launch(const ScheduledLaunch& launch);
  void finish_rocket(Rocket& rocket, bool detonated, bool killed, bool would_kill);
  FlightEnvironment flight_environment() const;

  WorldConfig config_;
  Atmosphere atmosphere_;
  WindField wind_;
  GustProcess gust_;
  std::vector<Target> targets_;
  OwnShip ship_;
  Radar radar_;
  Tracker tracker_;
  FireControlSolver solver_;
  std::vector<FireCommand> pending_;
  std::vector<SteerCommand> pending_steer_;
  std::vector<ScheduledLaunch> scheduled_;
  std::vector<Rocket> rockets_;
  std::vector<RocketResult> results_;
  std::vector<Event> events_;
  std::vector<TrackEvent> track_events_;
  std::vector<Plot> plots_scratch_;
  std::vector<math::Vec3> truths_scratch_;        // alive target truths for radar
  std::vector<math::Vec3> targets_prev_scratch_;  // pre-move target positions (CPA)
  Pcg32 dispersion_rng_;
  Pcg32 pk_rng_;
  std::uint64_t tick_ = 0;
  std::uint32_t next_rocket_id_ = 1;
};

}  // namespace seashield::sim
