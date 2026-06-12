#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "sim/world.h"

// Zero-dependency key=value scenario format. A scenario is (weather_seed +
// seeds + target + rocket + optional explicit weather overrides); together
// with the input journal it reproduces an entire engagement (charter §5.8).
//
//   # comment
//   weather_seed = 42
//   sim_seed = 7
//   gust_seed = 9
//   duration_s = 60
//   target_x = 6000  target_y/z, target_heading_deg, target_speed, target_turn_rate_deg
//   rocket_mass / rocket_cda / rocket_thrust / rocket_burn_time / rocket_fuze_radius
//   # explicit weather overrides (tests/experiments):
//   gravity, temperature_c, humidity, rain, turbulence,
//   surface_wind_speed, surface_wind_from_deg   (uniform wind column)
namespace seashield::sim {

struct Scenario {
  std::uint64_t weather_seed = 1;
  double duration_s = 60.0;
  // Server hint: stream FireSolution for confirmed tracks at this rate
  // (key fire_solution_rate_hz, 0 = off). Low by design — one PIP solve
  // integrates a full trajectory, so it must not ride the snapshot cadence.
  double fire_solution_rate_hz = 2.0;
  // Server hint: delta-compress snapshots for clients that ack (key
  // snapshot_delta, default on). Full snapshots remain the fallback for
  // silent/behind clients either way.
  bool snapshot_delta = true;
  WorldConfig config;
};

// Unknown keys are an error (typo safety). On failure returns nullopt and
// fills *error if provided.
std::optional<Scenario> load_scenario_text(const std::string& text, std::string* error);
std::optional<Scenario> load_scenario_file(const std::string& path, std::string* error);

}  // namespace seashield::sim
