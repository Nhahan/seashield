#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sim/scenario.h"

// Fire-control experiment harness core (charter §9 P4 산출물: 사격통제 성능
// 실험 — §2.4 "무유도 요격의 한계 정량화"). The sandbox stays a human demo
// CLI; this is the machine: a cartesian grid of scenario axes × repetitions,
// one CSV row per rocket, every row carrying the seeds that reproduce it.
//
// Determinism contract: a row is fully reproduced by (scenario, axis values,
// sim_seed, gust_seed) — no wall clock, no hidden state. The aggregate.py
// script reduces rows to per-cell statistics for the report.
namespace seashield::experiment {

// One sweep axis: name + explicit value list ("salvo=1,2,4" or "range=3000:15000:2000").
struct AxisSpec {
  std::string name;
  std::vector<double> values;
};

// Parses "name=start:end:step" or "name=v1,v2,...". nullopt on syntax error.
std::optional<AxisSpec> parse_axis(const std::string& text);

// Applies one axis value onto the scenario (or the fire parameters). False if
// the axis name is unknown — the caller reports it as a CLI error.
// Known axes: range, alt, speed, turn, weave, popup, salvo, dispersion,
//             wind, rain, accel_noise, sigma_range, settle.
struct CellParams {
  sim::Scenario scenario;
  int salvo = 4;
  double dispersion_mrad = 5.0;
  double settle_s = 6.0;
};
bool apply_axis_value(CellParams& cell, const std::string& name, double value);

// One rocket's outcome inside one engagement run.
struct RunRow {
  // Cell identity + reproduction key.
  std::vector<double> axis_values;
  int rep = 0;
  std::uint64_t sim_seed = 0;
  std::uint64_t gust_seed = 0;
  bool solver_track = false;
  // Kill-chain timeline (ticks; 0 = never happened).
  std::uint64_t first_track_tick = 0;
  std::uint64_t confirm_tick = 0;
  std::uint64_t launch_tick = 0;
  double track_error_at_launch_m = 0.0;  // |estimate - truth|; 0 in truth mode.
  double solver_tof_s = 0.0;
  bool fired = false;
  // Per-rocket adjudication (one row per rocket; rocket_id 0 + fired=false
  // when the engagement never launched).
  std::uint32_t rocket_id = 0;
  double miss_m = 0.0;
  bool detonated = false;
  bool killed = false;
  // Uncapped Pk roll (independent of the one-kill-per-engagement cap) — the
  // TRUE per-rocket kill probability for the §5 independence analysis.
  bool would_kill = false;
  bool salvo_killed = false;  // Any rocket of this run killed the target.
};

// Runs one engagement: waits for a confirmed track, lets it settle, fires one
// salvo from the chosen solution source, and adjudicates. Returns one row per
// rocket (or a single unfired row when no shot was possible).
std::vector<RunRow> run_engagement(const CellParams& cell, bool solver_track, int rep,
                                   std::uint64_t base_seed);

}  // namespace seashield::experiment
