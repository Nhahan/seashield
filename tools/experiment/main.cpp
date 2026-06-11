// Fire-control experiment harness CLI (charter §9 P4: 사격통제 성능 실험 —
// §2.4 "무유도 요격의 한계는 어디인가"를 정량 측정).
//
//   fc_experiment --axis range=4000:12000:2000 --axis salvo=1,2,4,8
//                 [--axis <name>=...]... [--reps 200] [--base-seed 1000]
//                 [--solver truth|track|both] [--settle-s 6] [--scenario FILE]
//                 --csv out.csv
//
// Axes: range alt speed turn weave popup salvo dispersion wind rain
//       accel_noise sigma_range settle
//
// One CSV row per rocket; every row carries (sim_seed, gust_seed), so any
// single row reproduces by itself. Aggregation: tools/experiment/aggregate.py.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "sim/scenario.h"
#include "tools/experiment/experiment.h"

namespace {

using namespace seashield;
using namespace seashield::experiment;

struct CliOptions {
  std::vector<AxisSpec> axes;
  int reps = 200;
  std::uint64_t base_seed = 1000;
  bool run_truth = false;
  bool run_track = true;
  double settle_s = 6.0;
  std::string scenario_path;
  std::string csv_path;
};

bool parse_args(int argc, char** argv, CliOptions& opts) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (arg == "--axis") {
      const char* v = next();
      if (v == nullptr) return false;
      const auto spec = parse_axis(v);
      if (!spec.has_value()) {
        std::fprintf(stderr, "잘못된 축 명세: %s\n", v);
        return false;
      }
      opts.axes.push_back(*spec);
    } else if (arg == "--reps") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.reps = static_cast<int>(std::strtol(v, nullptr, 10));
      if (opts.reps < 1) return false;
    } else if (arg == "--base-seed") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.base_seed = std::strtoull(v, nullptr, 10);
    } else if (arg == "--solver") {
      const char* v = next();
      if (v == nullptr) return false;
      const std::string mode = v;
      opts.run_truth = mode == "truth" || mode == "both";
      opts.run_track = mode == "track" || mode == "both";
      if (!opts.run_truth && !opts.run_track) return false;
    } else if (arg == "--settle-s") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.settle_s = std::strtod(v, nullptr);
    } else if (arg == "--scenario") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.scenario_path = v;
    } else if (arg == "--csv") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.csv_path = v;
    } else {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      return false;
    }
  }
  return !opts.csv_path.empty() && !opts.axes.empty();
}

void write_row(std::ofstream& out, const CliOptions& opts, const RunRow& row) {
  for (const double v : row.axis_values) {
    out << v << ',';
  }
  out << (row.solver_track ? "track" : "truth") << ',' << row.rep << ',' << row.sim_seed << ','
      << row.gust_seed << ',' << row.first_track_tick << ',' << row.confirm_tick << ','
      << row.launch_tick << ',' << row.track_error_at_launch_m << ',' << row.solver_tof_s << ','
      << (row.fired ? 1 : 0) << ',' << row.rocket_id << ',' << row.miss_m << ','
      << (row.detonated ? 1 : 0) << ',' << (row.killed ? 1 : 0) << ','
      << (row.salvo_killed ? 1 : 0) << '\n';
  (void)opts;
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions opts;
  if (!parse_args(argc, argv, opts)) {
    std::fprintf(stderr,
                 "usage: %s --axis name=spec [--axis ...] --csv FILE [--reps N] "
                 "[--base-seed N] [--solver truth|track|both] [--settle-s S] "
                 "[--scenario FILE]\n",
                 argv[0]);
    return 2;
  }

  // Base scenario: --scenario file or the built-in inbound-ASM baseline.
  std::string error;
  sim::Scenario base_scenario;
  if (!opts.scenario_path.empty()) {
    const auto loaded = sim::load_scenario_file(opts.scenario_path, &error);
    if (!loaded.has_value()) {
      std::fprintf(stderr, "시나리오 로드 실패: %s\n", error.c_str());
      return 1;
    }
    base_scenario = *loaded;
  } else {
    const auto loaded = sim::load_scenario_text(
        "weather_seed = 7\nduration_s = 90\n"
        "target_x = 0\ntarget_y = 9000\ntarget_z = 300\n"
        "target_heading_deg = 180\ntarget_speed = 250\n",
        &error);
    if (!loaded.has_value()) {
      std::fprintf(stderr, "기본 시나리오 구성 실패: %s\n", error.c_str());
      return 1;
    }
    base_scenario = *loaded;
  }

  std::ofstream csv(opts.csv_path);
  if (!csv.is_open()) {
    std::fprintf(stderr, "CSV 열기 실패: %s\n", opts.csv_path.c_str());
    return 1;
  }
  csv.precision(17);
  for (const AxisSpec& axis : opts.axes) {
    csv << axis.name << ',';
  }
  csv << "solver,rep,sim_seed,gust_seed,first_track_tick,confirm_tick,launch_tick,"
         "track_error_at_launch_m,solver_tof_s,fired,rocket_id,miss_m,detonated,killed,"
         "salvo_killed\n";

  // Cartesian grid over the axes (odometer indices).
  std::vector<std::size_t> index(opts.axes.size(), 0);
  std::size_t total_cells = 1;
  for (const AxisSpec& axis : opts.axes) {
    total_cells *= axis.values.size();
  }
  std::size_t cell_number = 0;
  std::uint64_t rows_written = 0;
  for (;;) {
    ++cell_number;
    CellParams cell;
    cell.scenario = base_scenario;
    cell.settle_s = opts.settle_s;
    std::vector<double> axis_values;
    bool cell_ok = true;
    for (std::size_t a = 0; a < opts.axes.size(); ++a) {
      const double value = opts.axes[a].values[index[a]];
      axis_values.push_back(value);
      if (!apply_axis_value(cell, opts.axes[a].name, value)) {
        std::fprintf(stderr, "알 수 없는 축: %s\n", opts.axes[a].name.c_str());
        cell_ok = false;
        break;
      }
    }
    if (!cell_ok) {
      return 2;
    }

    std::fprintf(stderr, "[%zu/%zu]", cell_number, total_cells);
    for (std::size_t a = 0; a < opts.axes.size(); ++a) {
      std::fprintf(stderr, " %s=%g", opts.axes[a].name.c_str(), axis_values[a]);
    }
    std::fprintf(stderr, "\n");

    for (int rep = 0; rep < opts.reps; ++rep) {
      for (const bool track_mode : {false, true}) {
        if ((track_mode && !opts.run_track) || (!track_mode && !opts.run_truth)) {
          continue;
        }
        auto rows = run_engagement(cell, track_mode, rep, opts.base_seed);
        for (RunRow& row : rows) {
          row.axis_values = axis_values;
          write_row(csv, opts, row);
          ++rows_written;
        }
      }
    }

    // Odometer increment.
    std::size_t a = 0;
    for (; a < opts.axes.size(); ++a) {
      if (++index[a] < opts.axes[a].values.size()) {
        break;
      }
      index[a] = 0;
    }
    if (a == opts.axes.size()) {
      break;
    }
  }

  std::printf("완료: 셀 %zu개 × 반복 %d, 행 %llu개 → %s\n", total_cells, opts.reps,
              static_cast<unsigned long long>(rows_written), opts.csv_path.c_str());
  return 0;
}
