#include "tools/experiment/experiment.h"

#include <cstdlib>
#include <map>

#include "sim/constants.h"
#include "sim/fire_control.h"
#include "sim/world.h"

namespace seashield::experiment {

std::optional<AxisSpec> parse_axis(const std::string& text) {
  const auto eq = text.find('=');
  if (eq == std::string::npos || eq == 0 || eq + 1 >= text.size()) {
    return std::nullopt;
  }
  AxisSpec spec;
  spec.name = text.substr(0, eq);
  const std::string values = text.substr(eq + 1);

  if (values.find(':') != std::string::npos) {
    double start = 0.0, end = 0.0, step = 0.0;
    if (std::sscanf(values.c_str(), "%lf:%lf:%lf", &start, &end, &step) != 3 || step <= 0.0 ||
        end < start) {
      return std::nullopt;
    }
    for (double v = start; v <= end + 1e-9; v += step) {
      spec.values.push_back(v);
    }
  } else {
    const char* cursor = values.c_str();
    char* end = nullptr;
    while (*cursor != '\0') {
      const double v = std::strtod(cursor, &end);
      if (end == cursor) {
        return std::nullopt;
      }
      spec.values.push_back(v);
      cursor = end;
      if (*cursor == ',') {
        ++cursor;
      } else if (*cursor != '\0') {
        return std::nullopt;
      }
    }
  }
  return spec.values.empty() ? std::nullopt : std::optional<AxisSpec>(spec);
}

bool apply_axis_value(CellParams& cell, const std::string& name, double value) {
  sim::WorldConfig& cfg = cell.scenario.config;
  if (name == "range") {
    // Inbound geometry: the target starts due north at this ground range.
    // The axis deliberately implies a due-south inbound course; combining it
    // with a heading-style axis would silently depend on axis order.
    cfg.target.initial_position = {0.0, value, cfg.target.initial_position.z};
    cfg.target.heading_rad = math::deg_to_rad(180.0);
    return true;
  }
  if (name == "alt") {
    cfg.target.initial_position.z = value;
    return true;
  }
  if (name == "speed") {
    cfg.target.speed_mps = value;
    return true;
  }
  if (name == "turn") {
    cfg.target.turn_rate_rad_s = math::deg_to_rad(value);
    return true;
  }
  if (name == "weave") {
    cfg.target.weave_range_m = value;  // 0 = off.
    return true;
  }
  if (name == "popup") {
    cfg.target.popup_range_m = value;  // 0 = off.
    return true;
  }
  if (name == "salvo") {
    cell.salvo = static_cast<int>(value);
    return true;
  }
  if (name == "dispersion") {
    cell.dispersion_mrad = value;
    return true;
  }
  if (name == "wind") {
    // Uniform westerly column (blowing toward +x), same shape the scenario
    // loader builds for surface_wind overrides.
    const math::Vec3 wind{value, 0.0, 0.0};
    cfg.weather.wind_layers = {{0.0, wind}, {6000.0, wind}};
    return true;
  }
  if (name == "rain") {
    cfg.weather.rain_intensity = value;  // Drag tweak + radar attenuation.
    return true;
  }
  if (name == "accel_noise") {
    cfg.tracker.accel_noise_mps2 = value;  // The Q axis of the Q/R study.
    return true;
  }
  if (name == "sigma_range") {
    cfg.radar.sigma_range_m = value;  // The R axis of the Q/R study.
    return true;
  }
  if (name == "settle") {
    cell.settle_s = value;  // Confirmation-to-fire wait (aim quality knob).
    return true;
  }
  return false;
}

std::vector<RunRow> run_engagement(const CellParams& cell, bool solver_track, int rep,
                                   std::uint64_t base_seed) {
  sim::WorldConfig config = cell.scenario.config;
  config.sim_seed = base_seed + static_cast<std::uint64_t>(rep);
  config.gust_seed = base_seed + 100000 + static_cast<std::uint64_t>(rep);

  sim::World world(config);
  const auto max_ticks =
      static_cast<std::uint64_t>(cell.scenario.duration_s * sim::kTickRateHz);
  const auto settle_ticks =
      static_cast<std::uint64_t>(cell.settle_s * sim::kTickRateHz);

  RunRow base;
  base.rep = rep;
  base.sim_seed = config.sim_seed;
  base.gust_seed = config.gust_seed;
  base.solver_track = solver_track;

  // Solver retries are throttled to a realistic fire-control cadence: one PIP
  // solve costs a full RK4 trajectory integration, so per-tick retries made
  // no-solution runs (e.g. hard-turning targets) ~30x slower than fired ones.
  // The first attempt still lands exactly on the settle-expiry tick.
  constexpr std::uint64_t kSolveRetryTicks = sim::kTickRateHz / 2;  // 0.5 s.
  std::map<std::uint32_t, std::uint64_t> confirm_seen;
  std::uint64_t next_attempt_tick = 0;
  bool fired = false;
  while (world.tick() < max_ticks && !(fired && world.ordnance_resolved())) {
    if (!fired) {
      for (const sim::Track& track : world.tracker().tracks()) {
        if (track.status != sim::TrackStatus::kConfirmed) {
          continue;
        }
        const auto [it, inserted] = confirm_seen.try_emplace(track.id, world.tick());
        if (world.tick() - it->second < settle_ticks || world.tick() < next_attempt_tick) {
          break;
        }
        next_attempt_tick = world.tick() + kSolveRetryTicks;
        // The truth branch fires at the SAME tick the track branch would —
        // identical timing isolates the aim-quality difference (the A/B of
        // charter §5.6 항목 1).
        std::optional<sim::FiringSolution> solution;
        if (solver_track) {
          solution = world.solve_for_track(track.id);
        } else {
          const sim::FireControlSolver solver(config.weather, config.rocket);
          solution = solver.solve(world.target().position(), world.target().velocity());
        }
        if (solution.has_value()) {
          sim::FireCommand cmd;
          cmd.azimuth_rad = solution->azimuth_rad;
          cmd.elevation_rad = solution->elevation_rad;
          cmd.salvo_count = cell.salvo;
          cmd.dispersion_mrad = cell.dispersion_mrad;
          world.queue_fire(cmd);
          fired = true;
          base.fired = true;
          base.launch_tick = world.tick();
          base.solver_tof_s = solution->time_of_flight_s;
          const sim::Track* fired_track = world.tracker().find(track.id);
          if (fired_track != nullptr) {
            base.track_error_at_launch_m =
                (fired_track->position() - world.target().position()).norm();
          }
        }
        break;
      }
    }
    world.step();
  }

  for (const sim::TrackEvent& event : world.track_events()) {
    if (event.kind == sim::TrackEvent::Kind::kInitiated && base.first_track_tick == 0) {
      base.first_track_tick = event.tick;
    }
    if (event.kind == sim::TrackEvent::Kind::kConfirmed && base.confirm_tick == 0) {
      base.confirm_tick = event.tick;
    }
  }

  std::vector<RunRow> rows;
  if (!base.fired || world.results().empty()) {
    rows.push_back(base);  // Unfired/unresolved run still yields one row.
    return rows;
  }
  const bool salvo_killed = world.target().destroyed();
  for (const sim::RocketResult& result : world.results()) {
    RunRow row = base;
    row.rocket_id = result.rocket_id;
    row.miss_m = result.miss_distance_m;
    row.detonated = result.detonated;
    row.killed = result.killed;
    row.would_kill = result.would_kill;
    row.salvo_killed = salvo_killed;
    rows.push_back(row);
  }
  return rows;
}

}  // namespace seashield::experiment
