// Dummy client CLI (P3b DoD: "더미 클라 N개가 스냅샷 수신" + 부하 테스트 1차).
//
//   dummyclient --port 7777 [--host 127.0.0.1] [--udp-port N] [--clients 4]
//               [--role observer|commander|weapons|solo] [--duration 5]
//               [--fire-after 1.0] [--fire-az-deg 38] [--fire-el-deg 12]
//               [--salvo 8] [--dispersion-mrad 5]
//               [--ack] [--fire-count N] [--fire-interval S]
//
// --ack assembles snapshots and acks them — the server then switches the
// client to the v4 delta stream (the production console's behaviour); the
// kbps column makes the full-vs-delta downlink comparison (P6 보고서).
// --fire-count/--fire-interval repeat volleys for stress entity counts.
//
// With N > 1, client 0 takes the requested role and the rest observe (the
// CIC has one weapons console but any number of spectators). --udp-port
// reroutes UDP through the chaos proxy while TCP stays direct.
// Exit code 0 = every client welcomed, bound, saw advancing snapshots, and
// observed no duplicate reliable event.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/math.h"
#include "net/socket_util.h"
#include "tools/dummy_client.h"

namespace {

struct Options {
  seashield::tools::DummyClientConfig client;
  int clients = 1;
};

bool parse_role(const std::string& name, seashield::protocol::Role& role) {
  using seashield::protocol::Role;
  if (name == "observer") role = Role::kObserver;
  else if (name == "commander") role = Role::kCommander;
  else if (name == "weapons") role = Role::kWeapons;
  else if (name == "solo") role = Role::kSolo;
  else return false;
  return true;
}

bool parse_args(int argc, char** argv, Options& opts) {
  double fire_az_deg = 0.0;
  double fire_el_deg = 30.0;
  bool fire_dir_set = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next_double = [&](double min, double max) -> double {
      if (i + 1 >= argc) return min - 1.0;
      const double v = std::strtod(argv[++i], nullptr);
      return (v < min || v > max) ? min - 1.0 : v;
    };
    auto next_long = [&](long min, long max) -> long {
      if (i + 1 >= argc) return min - 1;
      const long v = std::strtol(argv[++i], nullptr, 10);
      return (v < min || v > max) ? min - 1 : v;
    };
    if (arg == "--host") {
      if (i + 1 >= argc) return false;
      opts.client.host = argv[++i];
    } else if (arg == "--port") {
      const long v = next_long(1, 65535);
      if (v < 1) return false;
      opts.client.tcp_port = static_cast<std::uint16_t>(v);
    } else if (arg == "--udp-port") {
      const long v = next_long(1, 65535);
      if (v < 1) return false;
      opts.client.udp_port = static_cast<std::uint16_t>(v);
    } else if (arg == "--clients") {
      const long v = next_long(1, 64);
      if (v < 1) return false;
      opts.clients = static_cast<int>(v);
    } else if (arg == "--role") {
      if (i + 1 >= argc || !parse_role(argv[++i], opts.client.role)) return false;
    } else if (arg == "--duration") {
      const double v = next_double(0.1, 3600.0);
      if (v < 0.1) return false;
      opts.client.duration_s = v;
    } else if (arg == "--fire-after") {
      const double v = next_double(0.0, 3600.0);
      if (v < 0.0) return false;
      opts.client.fire_after_s = v;
    } else if (arg == "--fire-az-deg") {
      const double v = next_double(-360.0, 360.0);
      if (v < -360.0) return false;
      fire_az_deg = v;
      fire_dir_set = true;
    } else if (arg == "--fire-el-deg") {
      const double v = next_double(0.0, 90.0);
      if (v < 0.0) return false;
      fire_el_deg = v;
      fire_dir_set = true;
    } else if (arg == "--salvo") {
      const long v = next_long(1, 512);  // Stress runs go far past a real launcher.
      if (v < 1) return false;
      opts.client.fire.salvo_count = static_cast<std::uint16_t>(v);
    } else if (arg == "--dispersion-mrad") {
      const double v = next_double(0.0, 50.0);
      if (v < 0.0) return false;
      opts.client.fire.dispersion_mrad = v;
    } else if (arg == "--ack") {
      opts.client.ack_snapshots = true;
    } else if (arg == "--fire-count") {
      const long v = next_long(1, 64);
      if (v < 1) return false;
      opts.client.fire_count = static_cast<int>(v);
    } else if (arg == "--fire-interval") {
      const double v = next_double(0.01, 60.0);
      if (v < 0.01) return false;
      opts.client.fire_interval_s = v;
    } else {
      return false;
    }
  }
  if (fire_dir_set || opts.client.fire_after_s >= 0.0) {
    opts.client.fire.azimuth_rad = seashield::math::deg_to_rad(fire_az_deg);
    opts.client.fire.elevation_rad = seashield::math::deg_to_rad(fire_el_deg);
  }
  return opts.client.tcp_port != 0;
}

const char* role_name(seashield::protocol::Role role) {
  using seashield::protocol::Role;
  switch (role) {
    case Role::kObserver: return "observer";
    case Role::kCommander: return "commander";
    case Role::kWeapons: return "weapons";
    case Role::kSolo: return "solo";
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!parse_args(argc, argv, opts)) {
    std::fprintf(stderr,
                 "usage: %s --port N [--host H] [--udp-port N] [--clients N] [--role R] "
                 "[--duration S] [--fire-after S] [--fire-az-deg D] [--fire-el-deg D] "
                 "[--salvo N] [--dispersion-mrad M] [--ack] [--fire-count N] "
                 "[--fire-interval S]\n",
                 argv[0]);
    return 2;
  }
  seashield::net::ignore_sigpipe();

  std::vector<std::unique_ptr<seashield::tools::DummyClient>> clients;
  clients.reserve(static_cast<std::size_t>(opts.clients));
  for (int i = 0; i < opts.clients; ++i) {
    seashield::tools::DummyClientConfig config = opts.client;
    if (i > 0) {
      config.role = seashield::protocol::Role::kObserver;  // One armed seat, N spectators.
      config.fire_after_s = -1.0;
    }
    clients.push_back(std::make_unique<seashield::tools::DummyClient>(config));
  }

  std::vector<seashield::tools::DummyClientReport> reports(
      static_cast<std::size_t>(opts.clients));
  std::vector<std::thread> threads;
  threads.reserve(clients.size());
  for (std::size_t i = 0; i < clients.size(); ++i) {
    threads.emplace_back([&clients, &reports, i] { reports[i] = clients[i]->run(); });
  }
  for (auto& t : threads) {
    t.join();
  }

  bool ok = true;
  std::printf("\n%-7s %-10s %-10s %-8s %-8s %-9s %-9s %-9s %-7s %s\n", "client", "role",
              "snapshots", "ticks", "events", "asm/delta", "kB", "kbps", "dup", "status");
  for (std::size_t i = 0; i < reports.size(); ++i) {
    const auto& r = reports[i];
    // --ack clients assemble instead of counting raw full ticks, so accept
    // either liveness signal.
    const bool flowing = r.snapshot_ticks > 1 || r.assembled_ticks > 1;
    const bool healthy = r.welcomed && r.udp_bound && flowing && !r.duplicate_event &&
                         !r.disconnected_early && r.error.empty();
    ok = ok && healthy;
    char assembled[32];
    std::snprintf(assembled, sizeof(assembled), "%llu/%llu",
                  static_cast<unsigned long long>(r.assembled_ticks),
                  static_cast<unsigned long long>(r.delta_assembled_ticks));
    std::printf("%-7zu %-10s %-10llu %-8llu %-8zu %-9s %-9.1f %-9.1f %-7s %s\n", i,
                role_name(r.role), static_cast<unsigned long long>(r.snapshot_batches),
                static_cast<unsigned long long>(r.snapshot_ticks), r.events.size(), assembled,
                static_cast<double>(r.udp_bytes) / 1000.0,
                static_cast<double>(r.udp_bytes) * 8.0 / opts.client.duration_s / 1000.0,
                r.duplicate_event ? "YES" : "no",
                healthy          ? "ok"
                : !r.error.empty() ? r.error.c_str()
                                   : "FAILED");
  }
  if (!reports.empty() && reports[0].welcomed) {
    std::printf("\nweather: %s\nlast tick %u, %u entities, phase %s\n",
                reports[0].weather_summary.c_str(), reports[0].last_tick,
                std::max(reports[0].last_total_entities, reports[0].last_assembled_entities),
                reports[0].last_phase == seashield::protocol::EngagementPhase::kEnded ? "ended"
                                                                                      : "running");
  }
  return ok ? 0 : 1;
}
