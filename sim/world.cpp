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
      target_(config.target),
      radar_(config.radar, config.weather.rain_intensity, config.sim_seed),
      tracker_(config.tracker),
      solver_(config.weather, config.rocket),
      dispersion_rng_(config.sim_seed, /*stream=*/10),
      pk_rng_(config.sim_seed, /*stream=*/11) {}

void World::queue_fire(const FireCommand& command) { pending_.push_back(command); }

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
  rocket.state.position = kLaunchPosition;
  rocket.state.velocity = rocket.launch_dir * config_.rocket.rail_exit_speed_mps;
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

  const math::Vec3 target_prev = target_.position();
  target_.step(kTickDt);

  // (4b) Radar scan + (4c) tracker — the sensor chain observes the post-move
  // target (charter §4.6 순서: 물리→레이더→추적→…→판정). A destroyed target
  // returns no echoes, so its track coasts and dies naturally: kill
  // assessment is sensor-based too (charter §2.1).
  plots_scratch_.clear();
  if (!target_.destroyed()) {
    const math::Vec3 target_truth = target_.position();
    radar_.step(tick_, {&target_truth, 1}, plots_scratch_);
  } else {
    radar_.step(tick_, {}, plots_scratch_);
  }
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
    const math::Vec3 rel_before = rocket.state.position - target_prev;
    rocket_step(rocket.state, config_.rocket, env, rocket.launch_dir, kTickDt);
    const math::Vec3 rel_after = rocket.state.position - target_.position();

    // Linear relative motion within the tick (charter §5.7).
    const math::Vec3 rel_vel = (rel_after - rel_before) * kTickRateHz;
    const ClosestApproach cpa = closest_approach(rel_before, rel_vel, kTickDt);
    rocket.best_miss_m = std::min(rocket.best_miss_m, cpa.distance_m);

    if (cpa.distance_m < config_.rocket.proximity_fuze_radius_m) {
      // The fuze does not know whether the target already died: every
      // passage detonates and rolls its own Pk. would_kill is therefore the
      // UNCAPPED per-rocket kill metric (보고서 §5); killed keeps the
      // one-kill-per-engagement adjudication. The roll order is part of the
      // determinism contract — goldens regenerated with this change.
      const double pk = pk_from_miss(cpa.distance_m, config_.rocket.proximity_fuze_radius_m);
      const bool would_kill = pk_rng_.next_double() < pk;
      const bool killed = would_kill && !target_.destroyed();
      if (killed) {
        target_.destroy();
      }
      char buf[96];
      std::snprintf(buf, sizeof(buf), "rocket %u detonated: miss=%.1fm %s", rocket.id,
                    cpa.distance_m,
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
  auto solution = solver_.solve(track->position(), track->velocity());
  if (!solution.has_value()) {
    return fail(SolveForTrackError::kNoSolution);
  }
  return solution;
}

std::uint64_t World::state_hash() const {
  StateHasher hasher;
  hasher.mix(tick_);
  hasher.mix(target_.position());
  hasher.mix(target_.heading_rad());
  hasher.mix(target_.speed_mps());
  hasher.mix(target_.turn_rate_rad_s());
  hasher.mix(target_.destroyed());
  // ASM maneuver state (P4): the phase machine and the weave anchor are
  // mutable state the legacy fields above cannot reconstruct.
  hasher.mix(static_cast<std::uint64_t>(target_.phase()));
  hasher.mix(target_.weaving());
  hasher.mix(target_.weave_elapsed_s());
  // Queued/scheduled inputs are mutable state too: a replay that diverged in
  // command handling must not slip past the hash comparison.
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
