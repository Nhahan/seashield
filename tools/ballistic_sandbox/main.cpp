// Ballistic sandbox — the P2 demo CLI (charter §9 P4 실험 보고서의 씨앗).
//
// Generates a realistic random weather state from a seed, lets the operator
// (or the truth-based auto solution) pick firing parameters for an unguided
// rocket salvo, runs the deterministic engagement, and reports per-rocket
// miss distances.
//
//   ballistic_sandbox [--scenario FILE] [--weather-seed N] [--sim-seed N]
//                     [--auto | --az DEG --el DEG] [--offset-az DEG] [--offset-el DEG]
//                     [--salvo N] [--dispersion MRAD]
//                     [--dump trajectories.csv]
//                     [--compare-weather 1,2,3]
//                     [--sweep el=20:60:5 | --sweep az=30:50:2]
//
// 예시 (비전 데모): 같은 사격 제원, 다른 날씨 3종의 탄착 비교
//   ballistic_sandbox --auto --compare-weather 7,21,42

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "sim/fire_control.h"
#include "sim/scenario.h"
#include "sim/world.h"

namespace {

using namespace seashield;
using namespace seashield::sim;

struct SweepSpec {
  std::string param;  // "el" or "az"
  double start = 0;
  double end = 0;
  double step = 1;
};

struct CliOptions {
  std::string scenario_path;
  std::optional<std::uint64_t> weather_seed;
  std::optional<std::uint64_t> sim_seed;
  bool auto_solve = false;
  std::optional<double> az_deg;
  std::optional<double> el_deg;
  double offset_az_deg = 0.0;
  double offset_el_deg = 0.0;
  int salvo = 4;
  double dispersion_mrad = 4.0;
  std::string dump_path;
  std::vector<std::uint64_t> compare_seeds;
  std::optional<SweepSpec> sweep;
};

bool parse_args(int argc, char** argv, CliOptions& opts) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (arg == "--scenario") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.scenario_path = v;
    } else if (arg == "--weather-seed") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.weather_seed = std::strtoull(v, nullptr, 10);
    } else if (arg == "--sim-seed") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.sim_seed = std::strtoull(v, nullptr, 10);
    } else if (arg == "--auto") {
      opts.auto_solve = true;
    } else if (arg == "--az") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.az_deg = std::strtod(v, nullptr);
    } else if (arg == "--el") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.el_deg = std::strtod(v, nullptr);
    } else if (arg == "--offset-az") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.offset_az_deg = std::strtod(v, nullptr);
    } else if (arg == "--offset-el") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.offset_el_deg = std::strtod(v, nullptr);
    } else if (arg == "--salvo") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.salvo = static_cast<int>(std::strtol(v, nullptr, 10));
    } else if (arg == "--dispersion") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.dispersion_mrad = std::strtod(v, nullptr);
    } else if (arg == "--dump") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.dump_path = v;
    } else if (arg == "--compare-weather") {
      const char* v = next();
      if (v == nullptr) return false;
      char* cursor = const_cast<char*>(v);
      while (*cursor != '\0') {
        opts.compare_seeds.push_back(std::strtoull(cursor, &cursor, 10));
        if (*cursor == ',') {
          ++cursor;
        }
      }
    } else if (arg == "--sweep") {
      const char* v = next();
      if (v == nullptr) return false;
      SweepSpec spec;
      const char* eq = std::strchr(v, '=');
      if (eq == nullptr) return false;
      spec.param.assign(v, eq);
      if (spec.param != "el" && spec.param != "az") return false;
      if (std::sscanf(eq + 1, "%lf:%lf:%lf", &spec.start, &spec.end, &spec.step) != 3 ||
          spec.step <= 0.0) {
        return false;
      }
      opts.sweep = spec;
    } else {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      return false;
    }
  }
  if (!opts.auto_solve && (!opts.az_deg || !opts.el_deg) && !opts.sweep) {
    std::fprintf(stderr, "사격 제원이 없습니다: --auto 또는 --az/--el 또는 --sweep 필요\n");
    return false;
  }
  return true;
}

struct RunOutcome {
  std::vector<RocketResult> results;
  bool target_killed = false;
  double best_miss = 1e30;
  double end_time_s = 0.0;
};

RunOutcome run_engagement(const Scenario& scenario, const FireCommand& cmd,
                          std::ostream* dump) {
  World world(scenario.config);
  world.queue_fire(cmd);
  const auto max_ticks =
      static_cast<std::uint64_t>(scenario.duration_s * kTickRateHz);
  if (dump != nullptr) {
    *dump << "tick,entity,x,y,z\n";
  }
  while (!world.ordnance_resolved() && world.tick() < max_ticks) {
    world.step();
    if (dump != nullptr) {
      *dump << world.tick() << ",target," << world.target().position().x << ','
            << world.target().position().y << ',' << world.target().position().z << '\n';
      for (const Rocket& r : world.rockets()) {
        if (r.alive) {
          *dump << world.tick() << ",rocket" << r.id << ',' << r.state.position.x << ','
                << r.state.position.y << ',' << r.state.position.z << '\n';
        }
      }
    }
  }
  RunOutcome outcome;
  outcome.results = world.results();
  outcome.target_killed = world.target().destroyed();
  outcome.end_time_s = world.time_s();
  for (const RocketResult& r : outcome.results) {
    outcome.best_miss = std::min(outcome.best_miss, r.miss_distance_m);
  }
  return outcome;
}

// Resolves the firing command for a given weather (auto solution + offsets,
// or manual angles + offsets).
std::optional<FireCommand> resolve_fire(const CliOptions& opts, const Scenario& scenario,
                                        bool verbose) {
  FireCommand cmd;
  cmd.salvo_count = opts.salvo;
  cmd.dispersion_mrad = opts.dispersion_mrad;

  if (opts.auto_solve) {
    const Target probe(scenario.config.target);
    const FireControlSolver solver(scenario.config.weather, scenario.config.rocket);
    const auto solution =
        solver.solve(scenario.config.target.initial_position, probe.velocity());
    if (!solution.has_value()) {
      std::printf("자동 솔루션: 사거리 밖 — 해 없음\n");
      return std::nullopt;
    }
    if (verbose) {
      std::printf("자동 솔루션: az=%.2f° el=%.2f° (비행시간 %.1fs, 잔차 %.2fm, 탐색 %d회)\n",
                  math::rad_to_deg(solution->azimuth_rad),
                  math::rad_to_deg(solution->elevation_rad), solution->time_of_flight_s,
                  solution->predicted_miss_m, solution->probe_count);
    }
    cmd.azimuth_rad = solution->azimuth_rad;
    cmd.elevation_rad = solution->elevation_rad;
  } else {
    cmd.azimuth_rad = math::deg_to_rad(*opts.az_deg);
    cmd.elevation_rad = math::deg_to_rad(*opts.el_deg);
  }
  cmd.azimuth_rad += math::deg_to_rad(opts.offset_az_deg);
  cmd.elevation_rad += math::deg_to_rad(opts.offset_el_deg);
  return cmd;
}

void print_outcome(const RunOutcome& outcome) {
  std::printf("\n%-8s %-12s %-10s %s\n", "rocket", "miss(m)", "detonated", "result");
  for (const RocketResult& r : outcome.results) {
    std::printf("%-8u %-12.1f %-10s %s\n", r.rocket_id, r.miss_distance_m,
                r.detonated ? "yes" : "no", r.killed ? "KILL" : "-");
  }
  std::printf("요약: 최소 miss %.1fm · 표적 %s · 종료 %.1fs\n", outcome.best_miss,
              outcome.target_killed ? "격추" : "생존", outcome.end_time_s);
}

Scenario scenario_with_weather(Scenario base, std::uint64_t weather_seed) {
  base.weather_seed = weather_seed;
  base.config.weather = WeatherGenerator::generate(weather_seed);
  return base;
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions opts;
  if (!parse_args(argc, argv, opts)) {
    std::fprintf(stderr,
                 "usage: %s [--scenario FILE] [--weather-seed N] [--sim-seed N] "
                 "[--auto | --az DEG --el DEG] [--offset-az DEG] [--offset-el DEG] "
                 "[--salvo N] [--dispersion MRAD] [--dump FILE] "
                 "[--compare-weather 1,2,3] [--sweep el=20:60:5]\n",
                 argv[0]);
    return 2;
  }

  std::string error;
  std::optional<Scenario> scenario =
      opts.scenario_path.empty() ? load_scenario_text("", &error)
                                 : load_scenario_file(opts.scenario_path, &error);
  if (!scenario.has_value()) {
    std::fprintf(stderr, "시나리오 로드 실패: %s\n", error.c_str());
    return 1;
  }
  if (opts.weather_seed.has_value()) {
    *scenario = scenario_with_weather(*scenario, *opts.weather_seed);
  }
  if (opts.sim_seed.has_value()) {
    scenario->config.sim_seed = *opts.sim_seed;
  }

  const TargetParams& target = scenario->config.target;
  std::printf("표적: (%.0f, %.0f, %.0f)m, 침로 %.0f°, %.0fm/s%s\n",
              target.initial_position.x, target.initial_position.y,
              target.initial_position.z, math::rad_to_deg(target.heading_rad),
              target.speed_mps,
              target.turn_rate_rad_s != 0.0 ? " (선회 기동)" : "");

  // --compare-weather: 같은 사격 절차를 날씨 시드만 바꿔 반복 (비전 데모).
  if (!opts.compare_seeds.empty()) {
    std::printf("\n=== 날씨 비교: 동일 절차, weather-seed %zu종 ===\n",
                opts.compare_seeds.size());
    std::printf("%-8s %-12s %-10s %-8s %s\n", "seed", "best miss", "kill", "az/el", "날씨");
    for (const std::uint64_t seed : opts.compare_seeds) {
      const Scenario s = scenario_with_weather(*scenario, seed);
      const auto cmd = resolve_fire(opts, s, /*verbose=*/false);
      if (!cmd.has_value()) {
        std::printf("%-8llu %-12s %-10s %-8s %s\n", static_cast<unsigned long long>(seed),
                    "-", "-", "-", "해 없음");
        continue;
      }
      const RunOutcome outcome = run_engagement(s, *cmd, nullptr);
      char angles[32];
      std::snprintf(angles, sizeof(angles), "%.1f/%.1f", math::rad_to_deg(cmd->azimuth_rad),
                    math::rad_to_deg(cmd->elevation_rad));
      std::printf("%-8llu %-12.1f %-10s %-8s %s\n", static_cast<unsigned long long>(seed),
                  outcome.best_miss, outcome.target_killed ? "KILL" : "-", angles,
                  s.config.weather.describe().c_str());
    }
    return 0;
  }

  std::printf("기상(seed %llu): %s\n",
              static_cast<unsigned long long>(scenario->weather_seed),
              scenario->config.weather.describe().c_str());

  // --sweep: 한 파라미터를 훑으며 요격률 표 생성 (실험 보고서의 씨앗).
  if (opts.sweep.has_value()) {
    const SweepSpec& sweep = *opts.sweep;
    std::printf("\n=== 파라미터 스윕: %s = %.1f..%.1f (step %.1f) ===\n",
                sweep.param.c_str(), sweep.start, sweep.end, sweep.step);
    std::printf("%-10s %-12s %-12s %s\n", sweep.param.c_str(), "best miss", "detonations",
                "kill");
    for (double value = sweep.start; value <= sweep.end + 1e-9; value += sweep.step) {
      CliOptions point = opts;
      point.auto_solve = false;
      if (sweep.param == "el") {
        point.el_deg = value;
        if (!point.az_deg) {
          point.az_deg = math::rad_to_deg(
              math::atan2(target.initial_position.x, target.initial_position.y));
        }
      } else {
        point.az_deg = value;
        if (!point.el_deg) {
          point.el_deg = 30.0;
        }
      }
      const auto cmd = resolve_fire(point, *scenario, false);
      if (!cmd.has_value()) {
        continue;
      }
      const RunOutcome outcome = run_engagement(*scenario, *cmd, nullptr);
      int detonations = 0;
      for (const RocketResult& r : outcome.results) {
        detonations += r.detonated ? 1 : 0;
      }
      std::printf("%-10.1f %-12.1f %d/%-10d %s\n", value, outcome.best_miss, detonations,
                  opts.salvo, outcome.target_killed ? "KILL" : "-");
    }
    return 0;
  }

  // 단일 교전.
  const auto cmd = resolve_fire(opts, *scenario, /*verbose=*/true);
  if (!cmd.has_value()) {
    return 1;
  }
  if (opts.offset_az_deg != 0.0 || opts.offset_el_deg != 0.0) {
    std::printf("수동 보정: az %+.2f°, el %+.2f°\n", opts.offset_az_deg, opts.offset_el_deg);
  }
  std::printf("발사: az=%.2f° el=%.2f° 일제사 %d발 산포 %.1fmrad\n",
              math::rad_to_deg(cmd->azimuth_rad), math::rad_to_deg(cmd->elevation_rad),
              opts.salvo, opts.dispersion_mrad);

  std::ofstream dump_stream;
  std::ostream* dump = nullptr;
  if (!opts.dump_path.empty()) {
    dump_stream.open(opts.dump_path);
    if (dump_stream.is_open()) {
      dump = &dump_stream;
    }
  }
  const RunOutcome outcome = run_engagement(*scenario, *cmd, dump);
  print_outcome(outcome);
  if (dump != nullptr) {
    std::printf("궤적 CSV: %s\n", opts.dump_path.c_str());
  }
  return 0;
}
