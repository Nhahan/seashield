#include "sim/world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "sim/state_hash.h"

namespace seashield::sim {

World::World(const WorldConfig& config)
    : config_(config),
      atmosphere_(config.weather),
      wind_(config.weather.wind_layers),
      gust_(config.weather.turbulence_intensity * config.weather.surface_wind_speed(),
            config.gust_seed),
      radar_(config.radar, config.weather.rain_intensity, config.sim_seed),
      tracker_(config.tracker),
      solver_(config.weather, config.rocket),
      dispersion_rng_(config.sim_seed, /*stream=*/10),
      pk_rng_(config.sim_seed, /*stream=*/11) {
  // targets_[0] is the primary target (the legacy single-target world when
  // additional_targets is empty — bit-for-bit, the radar/Pk RNG order is
  // unchanged for N=1; only state_hash() gains a count field).
  targets_.emplace_back(config.target);
  for (const TargetParams& extra : config.additional_targets) {
    targets_.emplace_back(extra);
  }
  // Own ship: fixed platform at the origin unless the game mode configures a
  // steerable hull. Pure kinematics, so N=1 RNG order is unaffected.
  ship_.params = config.ship;
  ship_.position = config.ship.initial_position;
  ship_.heading_rad = config.ship.heading_rad;
  ship_.speed_mps = config.ship.speed_mps;
}

void OwnShip::step(double dt_s) {
  // Rudder slews the heading at a capped rate; throttle drives speed toward its
  // set-point under an acceleration limit. Both limits default to zero, which
  // pins a fixed platform at the origin (the legacy single-engagement world).
  if (params.turn_rate_max_rad_s > 0.0) {
    heading_rad += rudder * params.turn_rate_max_rad_s * dt_s;
  }
  const double target_speed = throttle * params.max_speed_mps;
  const double max_dv = params.accel_mps2 * dt_s;
  speed_mps += std::clamp(target_speed - speed_mps, -max_dv, max_dv);
  position.x += speed_mps * math::sin(heading_rad) * dt_s;
  position.y += speed_mps * math::cos(heading_rad) * dt_s;
}

void World::queue_fire(const FireCommand& command) { pending_.push_back(command); }

void World::queue_steer(const SteerCommand& command) { pending_steer_.push_back(command); }

FlightEnvironment World::flight_environment() const {
  FlightEnvironment env;
  env.atmosphere = &atmosphere_;
  env.wind = &wind_;
  env.gust = gust_.current();
  env.gravity_mps2 = config_.weather.gravity_mps2;
  env.rain_drag_multiplier = 1.0 + 0.02 * config_.weather.rain_intensity;
  return env;
}

void World::launch(const ScheduledLaunch& launch) {
  // Launcher dispersion: gaussian perturbation of the firing angles (mrad).
  const double sigma = math::mrad_to_rad(launch.dispersion_mrad);
  const double az = launch.azimuth_rad + dispersion_rng_.gaussian(0.0, sigma);
  const double el = launch.elevation_rad + dispersion_rng_.gaussian(0.0, sigma);

  Rocket rocket;
  rocket.id = next_rocket_id_++;
  rocket.launch_dir = math::direction_from_az_el(az, el);
  // Launch from the ship's deck; the rocket inherits the ship's velocity. With
  // a fixed platform this is kLaunchPosition and zero inheritance — identical
  // to the legacy single-engagement world.
  rocket.state.position = ship_.launch_position();
  rocket.state.velocity =
      rocket.launch_dir * config_.rocket.rail_exit_speed_mps + ship_.velocity();
  rockets_.push_back(rocket);
}

void World::finish_rocket(Rocket& rocket, bool detonated, bool killed, bool would_kill) {
  rocket.alive = false;
  RocketResult result;
  result.rocket_id = rocket.id;
  result.miss_distance_m = rocket.best_miss_m;
  result.detonated = detonated;
  result.killed = killed;
  result.would_kill = would_kill;
  result.end_tick = tick_;
  results_.push_back(result);
}

void World::step() {
  // (1) Consume queued commands: expand salvos into scheduled launches.
  // Scheduled ticks are always >= the current tick by construction (i >= 0),
  // and sub-tick launch intervals quantize UP to one tick — the scheduler
  // cannot fire twice within a tick (documented quantization).
  for (const FireCommand& cmd : pending_) {
    const auto interval_ticks = static_cast<std::uint64_t>(
        std::max<long long>(1, std::llround(cmd.launch_interval_s * kTickRateHz)));
    for (int i = 0; i < cmd.salvo_count; ++i) {
      scheduled_.push_back({tick_ + static_cast<std::uint64_t>(i) * interval_ticks,
                            cmd.azimuth_rad, cmd.elevation_rad, cmd.dispersion_mrad});
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "fire: salvo=%d az=%.2f° el=%.2f°", cmd.salvo_count,
                  math::rad_to_deg(cmd.azimuth_rad), math::rad_to_deg(cmd.elevation_rad));
    events_.push_back({tick_, buf});
  }
  pending_.clear();

  // (2) Launch rockets scheduled for this tick (stable order).
  for (const ScheduledLaunch& s : scheduled_) {
    if (s.tick == tick_) {
      launch(s);
    }
  }
  std::erase_if(scheduled_, [&](const ScheduledLaunch& s) { return s.tick <= tick_; });

  // (3) Advance the gust process, (4) the target, (5) the rockets in id order.
  gust_.step(kTickDt);

  // (3b) Own ship: latch the latest held steer set-points, then integrate
  // (no RNG). A fixed platform stays exactly at the origin, so the targets,
  // radar and engagement below see the legacy geometry bit-for-bit.
  for (const SteerCommand& s : pending_steer_) {
    ship_.rudder = s.rudder;
    ship_.throttle = s.throttle;
  }
  pending_steer_.clear();
  ship_.step(kTickDt);

  // (4) Step every target toward the ship's current pose; remember pre-move
  // positions for the CPA test.
  targets_prev_scratch_.clear();
  for (Target& t : targets_) {
    targets_prev_scratch_.push_back(t.position());
    t.step(kTickDt, ship_.position);
  }

  // (4b) Radar scan + (4c) tracker — the sensor chain observes the post-move
  // targets (charter §4.6 순서: 물리→레이더→추적→…→판정). Scans all ALIVE targets
  // (span order = targets_ order); a destroyed target returns no echoes so its
  // track coasts and dies naturally. A single alive target reproduces the
  // legacy detection RNG order bit-for-bit.
  plots_scratch_.clear();
  truths_scratch_.clear();
  for (const Target& t : targets_) {
    if (!t.destroyed()) {
      truths_scratch_.push_back(t.position());
    }
  }
  radar_.step(tick_, truths_scratch_, plots_scratch_, ship_.position);
  tracker_.predict();
  if (!plots_scratch_.empty()) {
    tracker_.update(plots_scratch_, tick_);
  }
  if (radar_.scan_index(tick_ + 1) != radar_.scan_index(tick_)) {
    tracker_.on_scan_boundary(tick_);
  }
  for (const TrackEvent& event : tracker_.drain_events()) {
    static constexpr const char* kKindNames[] = {"initiated", "confirmed", "dropped"};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "track %u %s", event.track_id,
                  kKindNames[static_cast<std::size_t>(event.kind)]);
    events_.push_back({tick_, buf});
    track_events_.push_back(event);
  }

  const FlightEnvironment env = flight_environment();

  for (Rocket& rocket : rockets_) {
    if (!rocket.alive) {
      continue;
    }
    const math::Vec3 rocket_before = rocket.state.position;
    rocket_step(rocket.state, config_.rocket, env, rocket.launch_dir, kTickDt);

    // Closest approach to the NEAREST target this tick (linear relative motion,
    // charter §5.7). For a single target this is bit-identical to the legacy
    // path (one CPA, one nearest); the per-target loop only adds geometry, no
    // RNG, until a detonation rolls Pk once against the nearest target.
    double best_cpa = 1e30;
    std::size_t best_idx = 0;
    for (std::size_t ti = 0; ti < targets_.size(); ++ti) {
      const math::Vec3 rel_before = rocket_before - targets_prev_scratch_[ti];
      const math::Vec3 rel_after = rocket.state.position - targets_[ti].position();
      const math::Vec3 rel_vel = (rel_after - rel_before) * kTickRateHz;
      const ClosestApproach cpa = closest_approach(rel_before, rel_vel, kTickDt);
      if (cpa.distance_m < best_cpa) {
        best_cpa = cpa.distance_m;
        best_idx = ti;
      }
    }
    rocket.best_miss_m = std::min(rocket.best_miss_m, best_cpa);

    if (best_cpa < config_.rocket.proximity_fuze_radius_m) {
      // The fuze does not know whether the nearest target already died: every
      // passage detonates and rolls its own Pk. would_kill is the UNCAPPED
      // per-rocket metric (보고서 §5); killed keeps the one-kill-per-target cap.
      // Roll order is part of the determinism contract — goldens regenerated.
      Target& hit = targets_[best_idx];
      const double pk = pk_from_miss(best_cpa, config_.rocket.proximity_fuze_radius_m);
      const bool would_kill = pk_rng_.next_double() < pk;
      const bool killed = would_kill && !hit.destroyed();
      if (killed) {
        hit.destroy();
      }
      char buf[96];
      std::snprintf(buf, sizeof(buf), "rocket %u detonated: miss=%.1fm %s", rocket.id, best_cpa,
                    killed       ? "KILL"
                    : would_kill ? "would-kill (target already dead)"
                                 : "no kill");
      events_.push_back({tick_, buf});
      finish_rocket(rocket, /*detonated=*/true, killed, would_kill);
      continue;
    }
    if (rocket.state.position.z <= 0.0 && rocket.state.age_s > 0.5) {
      finish_rocket(rocket, false, false, false);
      continue;
    }
    if (rocket.state.age_s >= config_.rocket.max_lifetime_s) {
      finish_rocket(rocket, false, false, false);
    }
  }

  ++tick_;
}

bool World::ordnance_resolved() const {
  if (!pending_.empty() || !scheduled_.empty()) {
    return false;
  }
  return std::none_of(rockets_.begin(), rockets_.end(),
                      [](const Rocket& r) { return r.alive; });
}

std::optional<FiringSolution> World::solve_for_track(std::uint32_t track_id,
                                                    SolveForTrackError* reason) const {
  const auto fail = [&](SolveForTrackError why) -> std::optional<FiringSolution> {
    if (reason != nullptr) {
      *reason = why;
    }
    return std::nullopt;
  };
  const Track* track = tracker_.find(track_id);
  if (track == nullptr) {
    return fail(SolveForTrackError::kNoSuchTrack);
  }
  if (track->status != TrackStatus::kConfirmed) {
    return fail(SolveForTrackError::kNotConfirmed);
  }
  if (track->consecutive_missed_scans >= tracker_.params().max_coast_scans) {
    return fail(SolveForTrackError::kStale);
  }
  // The estimated state slots straight into the truth-based solver: both are
  // a (position, velocity) pair under CV extrapolation. Sensor noise rides in
  // unannounced — exactly the error propagation P4 exists to measure.
  auto solution = solver_.solve(track->position(), track->velocity(), ship_.launch_position(),
                                ship_.velocity());
  if (!solution.has_value()) {
    return fail(SolveForTrackError::kNoSolution);
  }
  return solution;
}

std::uint64_t World::state_hash() const {
  StateHasher hasher;
  hasher.mix(tick_);
  // All concurrent targets (count + each). N=1 is the legacy single-target
  // world; the extra count field is why goldens regenerate for this change.
  hasher.mix(static_cast<std::uint64_t>(targets_.size()));
  for (const Target& t : targets_) {
    hasher.mix(t.position());
    hasher.mix(t.heading_rad());
    hasher.mix(t.speed_mps());
    hasher.mix(t.turn_rate_rad_s());
    hasher.mix(t.destroyed());
    // ASM maneuver state (P4): the phase machine and the weave anchor are
    // mutable state the legacy fields above cannot reconstruct.
    hasher.mix(static_cast<std::uint64_t>(t.phase()));
    hasher.mix(t.weaving());
    hasher.mix(t.weave_elapsed_s());
  }
  // Own-ship pose + held set-points. Constant (origin, zero) for the fixed
  // platform, but still mixed — the added fields are why goldens regenerate
  // for this change, and a moving ship must be reconstructible from the hash.
  hasher.mix(ship_.position);
  hasher.mix(ship_.heading_rad);
  hasher.mix(ship_.speed_mps);
  hasher.mix(ship_.rudder);
  hasher.mix(ship_.throttle);
  // Queued/scheduled inputs are mutable state too: a replay that diverged in
  // command handling must not slip past the hash comparison.
  hasher.mix(static_cast<std::uint64_t>(pending_steer_.size()));
  for (const SteerCommand& s : pending_steer_) {
    hasher.mix(s.rudder);
    hasher.mix(s.throttle);
  }
  hasher.mix(static_cast<std::uint64_t>(pending_.size()));
  for (const FireCommand& c : pending_) {
    hasher.mix(c.azimuth_rad);
    hasher.mix(c.elevation_rad);
    hasher.mix(static_cast<std::uint64_t>(c.salvo_count));
    hasher.mix(c.dispersion_mrad);
    hasher.mix(c.launch_interval_s);
  }
  hasher.mix(static_cast<std::uint64_t>(scheduled_.size()));
  for (const ScheduledLaunch& s : scheduled_) {
    hasher.mix(s.tick);
    hasher.mix(s.azimuth_rad);
    hasher.mix(s.elevation_rad);
    hasher.mix(s.dispersion_mrad);
  }
  hasher.mix(static_cast<std::uint64_t>(rockets_.size()));
  for (const Rocket& r : rockets_) {
    hasher.mix(static_cast<std::uint64_t>(r.id));
    hasher.mix(r.alive);
    hasher.mix(r.state.position);
    hasher.mix(r.state.velocity);
    hasher.mix(r.state.age_s);
    hasher.mix(r.best_miss_m);
  }
  hasher.mix(static_cast<std::uint64_t>(results_.size()));
  for (const RocketResult& res : results_) {
    hasher.mix(static_cast<std::uint64_t>(res.rocket_id));
    hasher.mix(res.miss_distance_m);
    hasher.mix(res.detonated);
    hasher.mix(res.killed);
    hasher.mix(res.would_kill);
    hasher.mix(res.end_tick);
  }
  hasher.mix(gust_.current());
  hasher.mix(gust_.rng_state());
  hasher.mix(dispersion_rng_.state());
  hasher.mix(pk_rng_.state());
  // Sensor chain (P4): radar RNG progress and the current-scan plot buffer...
  hasher.mix(radar_.detection_rng_state());
  hasher.mix(radar_.noise_rng_state());
  hasher.mix(static_cast<std::uint64_t>(radar_.last_scan_plots().size()));
  for (const Plot& plot : radar_.last_scan_plots()) {
    hasher.mix(plot.tick);
    hasher.mix(static_cast<std::uint64_t>(plot.scan_index));
    hasher.mix(plot.range_m);
    hasher.mix(plot.azimuth_rad);
    hasher.mix(plot.elevation_rad);
    hasher.mix(plot.position);
  }
  // ...and every track in full, INCLUDING all 36 covariance entries: an
  // asymmetry or numerical drift in P is a determinism defect and must trip
  // the golden regression, not hide behind a summary.
  hasher.mix(static_cast<std::uint64_t>(tracker_.next_track_id()));
  hasher.mix(static_cast<std::uint64_t>(tracker_.tracks().size()));
  for (const Track& track : tracker_.tracks()) {
    hasher.mix(static_cast<std::uint64_t>(track.id));
    hasher.mix(static_cast<std::uint64_t>(track.status));
    hasher.mix(track.last_update_tick);
    hasher.mix(static_cast<std::uint64_t>(track.scan_history));
    hasher.mix(static_cast<std::uint64_t>(track.consecutive_missed_scans));
    hasher.mix(track.updated_this_scan);
    for (int i = 0; i < 6; ++i) {
      hasher.mix(track.filter.state()[i]);
    }
    for (int r = 0; r < 6; ++r) {
      for (int c = 0; c < 6; ++c) {
        hasher.mix(track.filter.covariance()(r, c));
      }
    }
  }
  return hasher.value();
}

}  // namespace seashield::sim
