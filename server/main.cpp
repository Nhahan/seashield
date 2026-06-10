// SeaShield P1 demo server: framed TCP echo/broadcast + UDP echo.
//
//   seashield_server [--port 7777] [--udp-port 7778] [--mode broadcast|echo]
//                    [--send-cap 262144] [--max-clients 64] [--verbose]
//
// Demonstrates the P1 DoD: N concurrent clients on one authoritative loop,
// with slow clients evicted via the per-session send-queue cap (charter §4.8).

#include <arpa/inet.h>
#include <netinet/in.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/logger.h"
#include "core/unique_fd.h"
#include "net/acceptor.h"
#include "net/event_loop.h"
#include "net/socket_util.h"
#include "net/tcp_session.h"
#include "net/udp_endpoint.h"

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
    } else if (arg == "--verbose") {
      opts.verbose = true;
    } else {
      return false;
    }
  }
  return true;
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
                 "usage: %s [--port N] [--udp-port N] [--mode broadcast|echo] "
                 "[--send-cap BYTES] [--max-clients N] [--verbose]\n",
                 argv[0]);
    return 2;
  }
  if (opts.verbose) {
    seashield::log::set_min_level(seashield::log::Level::kDebug);
  }

  seashield::net::ignore_sigpipe();
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

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
