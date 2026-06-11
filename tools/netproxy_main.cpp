// UDP chaos proxy CLI (charter §10.3 "불량환경 주입"): put it between a
// client and the server's UDP port to inject seeded loss/duplication/latency.
//
//   netproxy --listen 7900 --upstream-port 7778 [--upstream-host 127.0.0.1]
//            [--loss 0.1] [--dup 0.05] [--delay-ms 40] [--jitter-ms 40]
//            [--seed 1] [--verbose]
//
// Point the dummy client's UDP traffic at --listen instead of the server.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/logger.h"
#include "net/socket_util.h"
#include "tools/net_chaos_proxy.h"

namespace {

seashield::tools::ChaosProxy* g_proxy = nullptr;
void handle_signal(int) {
  if (g_proxy != nullptr) {
    g_proxy->stop();
  }
}

bool parse_args(int argc, char** argv, seashield::tools::ChaosConfig& config, bool& verbose) {
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
    if (arg == "--listen") {
      const long v = next_long(0, 65535);
      if (v < 0) return false;
      config.listen_port = static_cast<std::uint16_t>(v);
    } else if (arg == "--upstream-host") {
      if (i + 1 >= argc) return false;
      config.upstream_host = argv[++i];
    } else if (arg == "--upstream-port") {
      const long v = next_long(1, 65535);
      if (v < 1) return false;
      config.upstream_port = static_cast<std::uint16_t>(v);
    } else if (arg == "--loss") {
      const double v = next_double(0.0, 1.0);
      if (v < 0.0) return false;
      config.loss = v;
    } else if (arg == "--dup") {
      const double v = next_double(0.0, 1.0);
      if (v < 0.0) return false;
      config.dup = v;
    } else if (arg == "--delay-ms") {
      const double v = next_double(0.0, 10000.0);
      if (v < 0.0) return false;
      config.delay_s = v / 1000.0;
    } else if (arg == "--jitter-ms") {
      const double v = next_double(0.0, 10000.0);
      if (v < 0.0) return false;
      config.jitter_s = v / 1000.0;
    } else if (arg == "--seed") {
      const long v = next_long(0, 0x7FFFFFFFL);
      if (v < 0) return false;
      config.seed = static_cast<std::uint64_t>(v);
    } else if (arg == "--verbose") {
      verbose = true;
    } else {
      return false;
    }
  }
  return config.upstream_port != 0;
}

}  // namespace

int main(int argc, char** argv) {
  seashield::tools::ChaosConfig config;
  bool verbose = false;
  if (!parse_args(argc, argv, config, verbose)) {
    std::fprintf(stderr,
                 "usage: %s --upstream-port N [--listen N] [--upstream-host H] [--loss P] "
                 "[--dup P] [--delay-ms N] [--jitter-ms N] [--seed N] [--verbose]\n",
                 argv[0]);
    return 2;
  }
  if (verbose) {
    seashield::log::set_min_level(seashield::log::Level::kDebug);
  }
  seashield::net::ignore_sigpipe();

  seashield::tools::ChaosProxy proxy(config);
  if (!proxy.init()) {
    SS_LOG_ERROR("failed to bind listen port %u", config.listen_port);
    return 1;
  }
  g_proxy = &proxy;
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  SS_LOG_INFO("chaos proxy: %u -> %s:%u  loss=%.2f dup=%.2f delay=%.0fms jitter=%.0fms seed=%llu",
              proxy.listen_port(), config.upstream_host.c_str(), config.upstream_port,
              config.loss, config.dup, config.delay_s * 1000.0, config.jitter_s * 1000.0,
              static_cast<unsigned long long>(config.seed));
  proxy.run();
  SS_LOG_INFO("chaos proxy: forwarded=%llu dropped=%llu duplicated=%llu",
              static_cast<unsigned long long>(proxy.forwarded()),
              static_cast<unsigned long long>(proxy.dropped()),
              static_cast<unsigned long long>(proxy.duplicated()));
  return 0;
}
