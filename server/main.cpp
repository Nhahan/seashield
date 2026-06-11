// SeaShield server.
//
// Simulation mode (P3): authoritative engagement server — TCP control channel
// (handshake/roles/fire commands) + UDP snapshot/event fan-out at 30Hz over a
// 60Hz fixed-tick world.
//
//   seashield_server --scenario scenarios/crossing-asm.scn
//                    [--port 7777] [--udp-port 7778] [--journal out.journal]
//                    [--send-cap 262144] [--max-clients 64] [--verbose]
//
// Echo mode (P1 demo, preserved): framed TCP echo/broadcast + UDP echo,
// demonstrating N concurrent clients with slow-client eviction (charter §4.8).
//
//   seashield_server [--port 7777] [--udp-port 7778] [--mode broadcast|echo]

#include <arpa/inet.h>
#include <netinet/in.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/logger.h"
#include "core/unique_fd.h"
#include "net/acceptor.h"
#include "net/event_loop.h"
#include "net/socket_util.h"
#include "net/tcp_session.h"
#include "net/udp_endpoint.h"
#include "server/sim_server.h"
#include "sim/scenario.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void handle_signal(int) { g_stop = 1; }

struct Options {
  std::uint16_t port = 7777;
  std::uint16_t udp_port = 7778;
  bool broadcast = true;
  std::size_t send_cap = 256 * 1024;
  std::size_t max_clients = 64;
  bool verbose = false;
  std::string scenario_path;  // Non-empty selects simulation mode.
  std::string journal_path;
  std::string replay_path;  // Non-empty: drive the sim from this journal.
};

bool parse_args(int argc, char** argv, Options& opts) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next_value = [&](long min, long max) -> long {
      if (i + 1 >= argc) {
        return -1;
      }
      const long value = std::strtol(argv[++i], nullptr, 10);
      return (value < min || value > max) ? -1 : value;
    };
    if (arg == "--port") {
      const long v = next_value(1, 65535);
      if (v < 0) return false;
      opts.port = static_cast<std::uint16_t>(v);
    } else if (arg == "--udp-port") {
      const long v = next_value(1, 65535);
      if (v < 0) return false;
      opts.udp_port = static_cast<std::uint16_t>(v);
    } else if (arg == "--mode") {
      if (i + 1 >= argc) return false;
      const std::string mode = argv[++i];
      if (mode == "broadcast") {
        opts.broadcast = true;
      } else if (mode == "echo") {
        opts.broadcast = false;
      } else {
        return false;
      }
    } else if (arg == "--send-cap") {
      const long v = next_value(1024, 64L * 1024 * 1024);
      if (v < 0) return false;
      opts.send_cap = static_cast<std::size_t>(v);
    } else if (arg == "--max-clients") {
      const long v = next_value(1, 4096);
      if (v < 0) return false;
      opts.max_clients = static_cast<std::size_t>(v);
    } else if (arg == "--scenario") {
      if (i + 1 >= argc) return false;
      opts.scenario_path = argv[++i];
    } else if (arg == "--journal") {
      if (i + 1 >= argc) return false;
      opts.journal_path = argv[++i];
    } else if (arg == "--replay") {
      if (i + 1 >= argc) return false;
      opts.replay_path = argv[++i];
    } else if (arg == "--verbose") {
      opts.verbose = true;
    } else {
      return false;
    }
  }
  return true;
}

int run_sim_mode(const Options& opts) {
  std::string error;
  const auto scenario = seashield::sim::load_scenario_file(opts.scenario_path, &error);
  if (!scenario) {
    SS_LOG_ERROR("failed to load scenario %s: %s", opts.scenario_path.c_str(), error.c_str());
    return 1;
  }
  seashield::server::SimServerConfig config;
  config.tcp_port = opts.port;
  config.udp_port = opts.udp_port;
  config.send_cap = opts.send_cap;
  config.max_clients = opts.max_clients;
  config.scenario = *scenario;
  config.journal_path = opts.journal_path;
  if (!opts.replay_path.empty()) {
    std::ifstream in(opts.replay_path);
    if (!in.is_open()) {
      SS_LOG_ERROR("cannot open replay journal %s", opts.replay_path.c_str());
      return 1;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    config.replay_journal_text = buffer.str();
    SS_LOG_INFO("replay mode: %s", opts.replay_path.c_str());
  }

  seashield::server::SimServer server(config);
  if (!server.start()) {
    SS_LOG_ERROR("failed to start sim server on tcp=%u/udp=%u", opts.port, opts.udp_port);
    return 1;
  }
  while (g_stop == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  server.stop();
  SS_LOG_INFO("sim server stopped: ticks=%llu snapshots=%llu events=%llu commands=%llu",
              static_cast<unsigned long long>(server.stats().ticks.load()),
              static_cast<unsigned long long>(server.stats().snapshot_batches_sent.load()),
              static_cast<unsigned long long>(server.stats().events_sent.load()),
              static_cast<unsigned long long>(server.stats().commands_accepted.load()));
  return 0;
}

class Server {
 public:
  Server(seashield::net::EventLoop& loop, const Options& opts) : loop_(loop), opts_(opts) {}

  bool init() {
    acceptor_ = std::make_unique<seashield::net::Acceptor>(
        loop_, [this](seashield::UniqueFd fd, const sockaddr_in& peer) {
          on_connection(std::move(fd), peer);
        });
    if (!acceptor_->listen(opts_.port)) {
      return false;
    }
    udp_ = std::make_unique<seashield::net::UdpEndpoint>(loop_);
    return udp_->open(opts_.udp_port,
                      [this](std::span<const std::uint8_t> payload, const sockaddr_in& from) {
                        udp_->send_to(payload, from);  // UDP echo.
                      });
  }

  // Deferred deletion + acceptor re-arm; runs between run_once() calls.
  void reap() {
    for (const std::uint64_t id : dead_) {
      sessions_.erase(id);
    }
    if (!dead_.empty() && acceptor_->paused()) {
      SS_LOG_INFO("descriptors freed; resuming accept");
      acceptor_->resume();
    }
    dead_.clear();
  }

  std::size_t session_count() const { return sessions_.size(); }

 private:
  void on_connection(seashield::UniqueFd fd, const sockaddr_in& peer) {
    char ip[INET_ADDRSTRLEN] = "?";
    ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    if (sessions_.size() >= opts_.max_clients) {
      SS_LOG_WARN("admission control: rejecting %s:%u (max-clients=%zu)", ip,
                  ntohs(peer.sin_port), opts_.max_clients);
      return;  // RAII closes the connection.
    }

    const std::uint64_t id = next_id_++;
    auto session =
        std::make_unique<seashield::net::TcpSession>(loop_, std::move(fd), id, opts_.send_cap);
    seashield::net::TcpSession* raw = session.get();
    sessions_[id] = std::move(session);
    raw->start(
        [this](seashield::net::TcpSession& s, std::span<const std::uint8_t> frame) {
          on_frame(s, frame);
        },
        [this](seashield::net::TcpSession& s, const char* reason) {
          SS_LOG_INFO("session %llu closed: %s", static_cast<unsigned long long>(s.id()), reason);
          dead_.push_back(s.id());
        });
    SS_LOG_INFO("session %llu connected from %s:%u (%zu online)",
                static_cast<unsigned long long>(id), ip, ntohs(peer.sin_port), sessions_.size());
  }

  void on_frame(seashield::net::TcpSession& from, std::span<const std::uint8_t> frame) {
    if (opts_.verbose) {
      SS_LOG_DEBUG("frame from session %llu: %zu bytes",
                   static_cast<unsigned long long>(from.id()), frame.size());
    }
    if (!opts_.broadcast) {
      from.send(frame);
      return;
    }
    // Broadcast: a send() may evict a slow session, which only marks it dead
    // (deferred deletion), so iterating the map stays safe.
    for (auto& [id, session] : sessions_) {
      session->send(frame);
    }
  }

  seashield::net::EventLoop& loop_;
  Options opts_;
  std::unique_ptr<seashield::net::Acceptor> acceptor_;
  std::unique_ptr<seashield::net::UdpEndpoint> udp_;
  std::unordered_map<std::uint64_t, std::unique_ptr<seashield::net::TcpSession>> sessions_;
  std::vector<std::uint64_t> dead_;
  std::uint64_t next_id_ = 1;
};

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!parse_args(argc, argv, opts)) {
    std::fprintf(stderr,
                 "usage: %s [--scenario FILE] [--journal FILE] [--replay FILE] [--port N] [--udp-port N] "
                 "[--mode broadcast|echo] [--send-cap BYTES] [--max-clients N] [--verbose]\n",
                 argv[0]);
    return 2;
  }
  if (opts.verbose) {
    seashield::log::set_min_level(seashield::log::Level::kDebug);
  }

  seashield::net::ignore_sigpipe();
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  if (!opts.scenario_path.empty()) {
    return run_sim_mode(opts);
  }

  auto loop = seashield::net::EventLoop::create();
  if (!loop) {
    SS_LOG_ERROR("failed to create event loop");
    return 1;
  }
  Server server(*loop, opts);
  if (!server.init()) {
    SS_LOG_ERROR("failed to bind tcp=%u/udp=%u", opts.port, opts.udp_port);
    return 1;
  }
  SS_LOG_INFO("seashield server up: tcp=%u udp=%u mode=%s send-cap=%zu max-clients=%zu", opts.port,
              opts.udp_port, opts.broadcast ? "broadcast" : "echo", opts.send_cap,
              opts.max_clients);

  while (g_stop == 0) {
    loop->run_once(100);
    server.reap();
  }
  SS_LOG_INFO("shutting down (%zu sessions online)", server.session_count());
  return 0;
}
