// Ballistic sandbox — the P2 demo CLI (charter §9 P4 실험 보고서의 씨앗).
//
// Generates a realistic random weather state from a seed, lets the operator
// (or the truth-based auto solution) pick firing parameters for an unguided
// rocket salvo, runs the deterministic engagement, and reports per-rocket
// miss distances.
//
//   ballistic_sandbox [--scenario FILE] [--weather-seed N] [--sim-seed N]
//                     [--auto | --az DEG --el DEG | --auto-track] [--offset-az DEG] [--offset-el DEG]
//                     [--salvo N] [--dispersion MRAD]
//                     [--dump trajectories.csv]
//                     [--compare-weather 1,2,3]
//                     [--sweep el=20:60:5 | --sweep az=30:50:2]
//                     [--journal-in FILE | --journal-out FILE]
//                     [--hash-every N] [--expect-final-hash HEX]
//
// 예시 (비전 데모): 같은 사격 제원, 다른 날씨 3종의 탄착 비교
//   ballistic_sandbox --auto --compare-weather 7,21,42
//
// P4 킬체인 데모 + 리플레이 (charter §5.8, §9 P4 DoD): 레이더가 추적을 확정하면
// 추정 트랙으로 자동 사격하고 그 명령을 저널로 남긴 뒤, 같은 저널을 되돌려
// 비트 동일 재현을 해시로 검증한다.
//   ballistic_sandbox --auto-track --journal-out run.journal --hash-every 120
//   ballistic_sandbox --journal-in run.journal --expect-final-hash <HEX>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "sim/fire_control.h"
#include "sim/journal.h"
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
  bool auto_track = false;
  double settle_s = 6.0;  // Wait after confirmation before auto-track fire.
  std::string journal_in_path;
  std::string journal_out_path;
  std::uint64_t hash_every = 0;
  std::optional<std::uint64_t> expect_final_hash;
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
    } else if (arg == "--auto-track") {
      opts.auto_track = true;
    } else if (arg == "--settle-s") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.settle_s = std::strtod(v, nullptr);
    } else if (arg == "--journal-in") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.journal_in_path = v;
    } else if (arg == "--journal-out") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.journal_out_path = v;
    } else if (arg == "--hash-every") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.hash_every = std::strtoull(v, nullptr, 10);
    } else if (arg == "--expect-final-hash") {
      const char* v = next();
      if (v == nullptr) return false;
      opts.expect_final_hash = std::strtoull(v, nullptr, 16);
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
  if (!opts.auto_solve && (!opts.az_deg || !opts.el_deg) && !opts.sweep && !opts.auto_track &&
      opts.journal_in_path.empty()) {
    std::fprintf(stderr,
                 "사격 제원이 없습니다: --auto, --az/--el, --sweep, --auto-track, "
                 "--journal-in 중 하나 필요\n");
    return false;
  }
  if (opts.auto_track && !opts.journal_in_path.empty()) {
    std::fprintf(stderr, "--auto-track과 --journal-in은 동시에 쓸 수 없습니다\n");
    return false;
  }
  return true;
}

struct RunOutcome {
  std::vector<RocketResult> results;
  bool target_killed = false;
  double best_miss = 1e30;
  double end_time_s = 0.0;
  std::uint64_t final_hash = 0;
  bool fired = false;
  std::uint64_t launch_tick = 0;
  std::uint64_t track_initiated_tick = 0;
  std::uint64_t track_confirmed_tick = 0;
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

// Full kill-chain runner (P4): replays journal entries at their recorded
// ticks, or — in auto-track mode — fires on the first confirmed track using
// the ESTIMATED state, journaling the command it issued. Either way the run
// is fully determined by (scenario, journal), which is what --expect-final-hash
// verifies (charter §5.8: 기록은 시나리오+시드+저널이 전부).
RunOutcome run_kill_chain(const Scenario& scenario, const CliOptions& opts,
                          const Journal* replay, Journal* record, std::ostream* dump) {
  World world(scenario.config);
  const auto max_ticks = static_cast<std::uint64_t>(scenario.duration_s * kTickRateHz);
  RunOutcome outcome;
  std::size_t next_entry = 0;
  std::map<std::uint32_t, std::uint64_t> confirm_seen;  // track id -> first confirmed tick.
  if (dump != nullptr) {
    *dump << "tick,entity,x,y,z\n";
  }

  while (world.tick() < max_ticks && !(outcome.fired && world.ordnance_resolved())) {
    const std::uint64_t t = world.tick();
    if (replay != nullptr) {
      while (next_entry < replay->entries().size() &&
             replay->entries()[next_entry].tick == t) {
        world.queue_fire(replay->entries()[next_entry].command);
        outcome.fired = true;
        outcome.launch_tick = t;
        ++next_entry;
      }
    } else if (opts.auto_track && !outcome.fired) {
      // Fire on the lowest-id confirmed track once its estimate has settled —
      // the headless demo of 탐지→추적→사격통제→판정 (charter §9 P4 DoD).
      // Firing at the instant of confirmation aims with a 3-scan-old velocity
      // estimate and misses by hundreds of meters (measured); the settle wait
      // trades reaction time for aim quality — an experiment axis in itself.
      for (const Track& track : world.tracker().tracks()) {
        if (track.status != TrackStatus::kConfirmed) {
          continue;
        }
        const auto [it, inserted] = confirm_seen.try_emplace(track.id, t);
        if (static_cast<double>(t - it->second) * kTickDt < opts.settle_s) {
          break;
        }
        const auto solution = world.solve_for_track(track.id);
        if (solution.has_value()) {
          FireCommand cmd;
          cmd.azimuth_rad = solution->azimuth_rad + math::deg_to_rad(opts.offset_az_deg);
          cmd.elevation_rad = solution->elevation_rad + math::deg_to_rad(opts.offset_el_deg);
          cmd.salvo_count = opts.salvo;
          cmd.dispersion_mrad = opts.dispersion_mrad;
          if (record != nullptr) {
            record->record(t, cmd);
          }
          world.queue_fire(cmd);
          outcome.fired = true;
          outcome.launch_tick = t;
          std::printf("자동 추적 사격: track %u, az=%.2f° el=%.2f° (비행시간 %.1fs, t=%.1fs)\n",
                      track.id, math::rad_to_deg(cmd.azimuth_rad),
                      math::rad_to_deg(cmd.elevation_rad), solution->time_of_flight_s,
                      static_cast<double>(t) * kTickDt);
        }
        break;  // Only the first confirmed track is considered per tick.
      }
    }

    world.step();

    if (opts.hash_every != 0 && world.tick() % opts.hash_every == 0) {
      std::printf("tick=%llu hash=%016llx\n",
                  static_cast<unsigned long long>(world.tick()),
                  static_cast<unsigned long long>(world.state_hash()));
    }
    if (dump != nullptr) {
      *dump << world.tick() << ",target," << world.target().position().x << ','
            << world.target().position().y << ',' << world.target().position().z << '\n';
      for (const Track& track : world.tracker().tracks()) {
        *dump << world.tick() << ",track" << track.id << ',' << track.position().x << ','
              << track.position().y << ',' << track.position().z << '\n';
      }
      for (const Rocket& r : world.rockets()) {
        if (r.alive) {
          *dump << world.tick() << ",rocket" << r.id << ',' << r.state.position.x << ','
                << r.state.position.y << ',' << r.state.position.z << '\n';
        }
      }
    }
  }

  for (const TrackEvent& event : world.track_events()) {
    if (event.kind == TrackEvent::Kind::kInitiated && outcome.track_initiated_tick == 0) {
      outcome.track_initiated_tick = event.tick;
    }
    if (event.kind == TrackEvent::Kind::kConfirmed && outcome.track_confirmed_tick == 0) {
      outcome.track_confirmed_tick = event.tick;
    }
  }
  outcome.results = world.results();
  outcome.target_killed = world.target().destroyed();
  outcome.end_time_s = world.time_s();
  outcome.final_hash = world.state_hash();
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

  // 단일 교전 — 수동/--auto는 "틱 0짜리 저널"로 환원되어 --auto-track,
  // --journal-in과 같은 킬체인 러너로 수렴한다: 어떤 경로든 런을 결정하는
  // 입력은 결국 (시나리오, 저널)이다 (charter §5.8).
  Journal journal;
  bool replay_mode = false;
  if (!opts.journal_in_path.empty()) {
    std::ifstream in(opts.journal_in_path);
    if (!in.is_open()) {
      std::fprintf(stderr, "저널 열기 실패: %s\n", opts.journal_in_path.c_str());
      return 1;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const auto parsed = Journal::parse(buffer.str());
    if (!parsed.has_value()) {
      std::fprintf(stderr, "저널 파싱 실패: %s\n", opts.journal_in_path.c_str());
      return 1;
    }
    journal = *parsed;
    replay_mode = true;
    std::printf("리플레이: %s (명령 %zu개)\n", opts.journal_in_path.c_str(),
                journal.entries().size());
  } else if (!opts.auto_track) {
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
    journal.record(0, *cmd);
    replay_mode = true;
  }

  std::ofstream dump_stream;
  std::ostream* dump = nullptr;
  if (!opts.dump_path.empty()) {
    dump_stream.open(opts.dump_path);
    if (dump_stream.is_open()) {
      dump = &dump_stream;
    }
  }

  Journal recorded;
  const RunOutcome outcome =
      run_kill_chain(*scenario, opts, replay_mode ? &journal : nullptr,
                     opts.auto_track ? &recorded : nullptr, dump);

  if (outcome.track_initiated_tick != 0 || outcome.track_confirmed_tick != 0) {
    char launch_label[24] = "없음";
    if (outcome.fired) {
      std::snprintf(launch_label, sizeof(launch_label), "%.1fs",
                    static_cast<double>(outcome.launch_tick) * kTickDt);
    }
    std::printf("킬체인: 트랙 개시 %.1fs → 확정 %.1fs → 발사 %s → 종결 %.1fs\n",
                static_cast<double>(outcome.track_initiated_tick) * kTickDt,
                static_cast<double>(outcome.track_confirmed_tick) * kTickDt, launch_label,
                outcome.end_time_s);
  }
  if (opts.auto_track && !outcome.fired) {
    std::fprintf(stderr, "자동 추적 사격 실패: 교전 시간 내 확정 트랙/사격해 없음\n");
  }

  print_outcome(outcome);
  std::printf("최종 해시: %016llx\n", static_cast<unsigned long long>(outcome.final_hash));

  if (!opts.journal_out_path.empty()) {
    const Journal& out_journal = opts.auto_track ? recorded : journal;
    std::ofstream out(opts.journal_out_path);
    out << out_journal.serialize();
    std::printf("저널 기록: %s (명령 %zu개)\n", opts.journal_out_path.c_str(),
                out_journal.entries().size());
  }
  if (dump != nullptr) {
    std::printf("궤적 CSV: %s\n", opts.dump_path.c_str());
  }
  if (opts.expect_final_hash.has_value() && *opts.expect_final_hash != outcome.final_hash) {
    std::fprintf(stderr, "해시 불일치: 기대 %016llx, 실제 %016llx\n",
                 static_cast<unsigned long long>(*opts.expect_final_hash),
                 static_cast<unsigned long long>(outcome.final_hash));
    return 1;
  }
  return (opts.auto_track && !outcome.fired) ? 1 : 0;
}
