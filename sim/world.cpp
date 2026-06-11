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

void World::finish_rocket(Rocket& rocket, bool detonated, bool killed) {
  rocket.alive = false;
  RocketResult result;
  result.rocket_id = rocket.id;
  result.miss_distance_m = rocket.best_miss_m;
  result.detonated = detonated;
  result.killed = killed;
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

    if (!target_.destroyed() && cpa.distance_m < config_.rocket.proximity_fuze_radius_m) {
      const double pk = pk_from_miss(cpa.distance_m, config_.rocket.proximity_fuze_radius_m);
      const bool killed = pk_rng_.next_double() < pk;
      if (killed) {
        target_.destroy();
      }
      char buf[96];
      std::snprintf(buf, sizeof(buf), "rocket %u detonated: miss=%.1fm %s", rocket.id,
                    cpa.distance_m, killed ? "KILL" : "no kill");
      events_.push_back({tick_, buf});
      finish_rocket(rocket, /*detonated=*/true, killed);
      continue;
    }
    if (rocket.state.position.z <= 0.0 && rocket.state.age_s > 0.5) {
      finish_rocket(rocket, false, false);
      continue;
    }
    if (rocket.state.age_s >= config_.rocket.max_lifetime_s) {
      finish_rocket(rocket, false, false);
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
    hasher.mix(res.end_tick);
  }
  hasher.mix(gust_.current());
  hasher.mix(gust_.rng_state());
  hasher.mix(dispersion_rng_.state());
  hasher.mix(pk_rng_.state());
  return hasher.value();
}

}  // namespace seashield::sim
