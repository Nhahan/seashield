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
  // Survival GAME mode (key game_mode, default off). When on, the server runs
  // an endless sequence of single-target waves instead of one frozen
  // engagement: each wave reseeds the world (key game_seed_stride applied to
  // sim/gust/weather seeds), a target that reaches the ship costs a life, and
  // the run ends when lives hit zero. The deterministic single-engagement path
  // (replay, golden, every existing test) is completely unaffected — this is a
  // strictly additive branch in the sim thread (charter §5.8 stays per-world).
  bool game_mode = false;
  int game_lives = 3;
  std::uint64_t game_seed_stride = 2654435761ULL;  // Knuth multiplicative spread.
  WorldConfig config;
};

// Unknown keys are an error (typo safety). On failure returns nullopt and
// fills *error if provided.
std::optional<Scenario> load_scenario_text(const std::string& text, std::string* error);
std::optional<Scenario> load_scenario_file(const std::string& path, std::string* error);

}  // namespace seashield::sim
