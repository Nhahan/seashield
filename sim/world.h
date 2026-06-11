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

struct WorldConfig {
  Weather weather;
  RocketParams rocket;
  TargetParams target;
  RadarParams radar;
  TrackerParams tracker;
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

  void step();

  std::uint64_t tick() const { return tick_; }
  double time_s() const { return static_cast<double>(tick_) * kTickDt; }

  const Weather& weather() const { return config_.weather; }
  const Target& target() const { return target_; }
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
  void finish_rocket(Rocket& rocket, bool detonated, bool killed);
  FlightEnvironment flight_environment() const;

  WorldConfig config_;
  Atmosphere atmosphere_;
  WindField wind_;
  GustProcess gust_;
  Target target_;
  Radar radar_;
  Tracker tracker_;
  FireControlSolver solver_;
  std::vector<FireCommand> pending_;
  std::vector<ScheduledLaunch> scheduled_;
  std::vector<Rocket> rockets_;
  std::vector<RocketResult> results_;
  std::vector<Event> events_;
  std::vector<TrackEvent> track_events_;
  std::vector<Plot> plots_scratch_;
  Pcg32 dispersion_rng_;
  Pcg32 pk_rng_;
  std::uint64_t tick_ = 0;
  std::uint32_t next_rocket_id_ = 1;
};

}  // namespace seashield::sim
