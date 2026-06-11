#pragma once

#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/pcg32.h"
#include "core/spsc_queue.h"
#include "net/acceptor.h"
#include "net/event_loop.h"
#include "net/tcp_session.h"
#include "net/udp_endpoint.h"
#include "protocol/messages.h"
#include "protocol/reliable.h"
#include "sim/journal.h"
#include "sim/scenario.h"

// P3b server assembly (charter §4.1/§4.6): net + protocol + sim glued into
// the two-thread shape —
//
//   [I/O thread]                            [Simulation thread, 60Hz]
//     EventLoop run_once + pump()             drain commands -> journal
//     TCP handshake/roles/commands            world.step()
//     UDP bind + snapshot/event fan-out       snapshot @30Hz + events
//             ▲            │                          │
//             └── SPSC ────┴────────── SPSC ◄─────────┘   (+ wakeup)
//
// The sim thread owns World/Journal and never sees a socket (charter §4.1);
// the I/O thread owns every session object (P1 thread-affinity contract).
// The ONLY shared state is the two SPSC queues.
namespace seashield::server {

struct SimServerConfig {
  std::uint16_t tcp_port = 0;  // 0 = ephemeral (tests).
  std::uint16_t udp_port = 0;
  std::size_t send_cap = 256 * 1024;
  std::size_t max_clients = 64;
  double handshake_timeout_s = 5.0;
  // Silence ceiling for the reliable channel: a client that cannot confirm
  // events for this long loses its UDP binding (tests shorten it).
  double reliable_peer_timeout_s = 10.0;
  sim::Scenario scenario;
  std::string journal_path;  // Non-empty: dump the input journal on stop().
};

// Counters are atomics so tests can observe progress from outside the I/O
// thread without touching server internals.
struct SimServerStats {
  std::atomic<std::uint64_t> ticks{0};
  std::atomic<std::uint64_t> snapshot_batches_sent{0};
  std::atomic<std::uint64_t> events_sent{0};
  std::atomic<std::uint64_t> commands_accepted{0};
  std::atomic<std::uint64_t> commands_rejected{0};
  std::atomic<std::uint64_t> sim_output_dropped{0};
  std::atomic<std::uint64_t> udp_unbound_timeouts{0};
  std::atomic<std::uint64_t> sessions_created{0};
  std::atomic<std::uint64_t> sessions_reattached{0};
  // First-cut tick-cost measurement (charter §9 P3 "부하 테스트 1차"): time
  // spent per simulation tick on work (drain/step/events/snapshot/publish),
  // sleep excluded. The full p99/jitter report is P6's job (§10.3).
  std::atomic<std::uint64_t> tick_busy_sum_us{0};
  std::atomic<std::uint64_t> tick_busy_max_us{0};
  std::atomic<std::uint64_t> tick_busy_over_8ms{0};  // §10.3 budget guardrail.
};

class SimServer {
 public:
  explicit SimServer(SimServerConfig config);
  ~SimServer();

  SimServer(const SimServer&) = delete;
  SimServer& operator=(const SimServer&) = delete;

  // Binds both ports and spawns the I/O and simulation threads.
  bool start();
  // Idempotent; joins both threads, then dumps the journal if configured.
  void stop();

  std::uint16_t tcp_port() const { return tcp_port_; }
  std::uint16_t udp_port() const { return udp_port_; }
  const SimServerStats& stats() const { return stats_; }
  // Serialized input journal; call after stop() (the sim thread owns it).
  std::string journal_text() const;

 private:
  // net -> sim: validated operator inputs.
  struct SimCommand {
    sim::FireCommand fire;
  };

  // sim -> net: one tick's outbound bundle.
  struct SimOutput {
    bool has_snapshot = false;
    std::uint32_t tick = 0;
    protocol::EngagementPhase phase = protocol::EngagementPhase::kRunning;
    std::vector<protocol::EntityRecord> entities;
    std::vector<protocol::EngagementEvent> events;
  };

  // Logical (operator) session: outlives its TCP transport so a reconnect
  // with the token restores the role (charter §4.8; design doc §6.4 P3 note).
  // The reliable endpoint is per-incarnation: a reconnecting client starts a
  // fresh endpoint, so the server resets its side too (sequence spaces must
  // be born together).
  struct LogicalSession {
    std::uint64_t token = 0;
    protocol::Role role = protocol::Role::kObserver;
    std::uint64_t transport_id = 0;  // 0 = detached (권한 잠금).
    bool udp_bound = false;
    sockaddr_in udp_addr{};
    std::unique_ptr<protocol::ReliableEndpoint> endpoint;
    double last_udp_seen_s = 0.0;
  };

  // --- I/O thread ---
  void io_thread_main();
  void pump();
  double now_s() const;
  void on_connection(UniqueFd fd, const sockaddr_in& peer);
  void on_frame(net::TcpSession& transport, std::span<const std::uint8_t> frame);
  void on_transport_closed(net::TcpSession& transport, const char* reason);
  void handle_hello(net::TcpSession& transport, const protocol::ClientHello& hello);
  void handle_fire(net::TcpSession& transport, const protocol::FireRequest& fire);
  void reject_and_close(net::TcpSession& transport, protocol::RejectReason reason);
  void on_udp_datagram(std::span<const std::uint8_t> payload, const sockaddr_in& from);
  // Stateless scan of an unknown-address datagram for a UdpHello token
  // (packet header + unreliable envelopes are parseable without per-session
  // state); binds the address on success.
  void try_udp_bind(std::span<const std::uint8_t> payload, const sockaddr_in& from);
  void route_data_message(LogicalSession& session, protocol::MsgType type,
                          std::span<const std::uint8_t> payload);
  void dispatch_sim_output(const SimOutput& output);
  void flush_session(LogicalSession& session, double now);
  void unbind_udp(LogicalSession& session, const char* reason);
  void reap_transports();
  std::unique_ptr<protocol::ReliableEndpoint> make_endpoint() const;
  std::uint64_t make_token();
  bool role_available(protocol::Role role) const;
  static std::uint64_t addr_key(const sockaddr_in& addr);

  // --- simulation thread ---
  void sim_thread_main();

  SimServerConfig config_;
  SimServerStats stats_;

  std::unique_ptr<net::EventLoop> loop_;
  std::unique_ptr<net::Acceptor> acceptor_;
  std::unique_ptr<net::UdpEndpoint> udp_;
  std::uint16_t tcp_port_ = 0;
  std::uint16_t udp_port_ = 0;

  // I/O-thread state (thread affinity per design doc §7).
  std::unordered_map<std::uint64_t, std::unique_ptr<net::TcpSession>> transports_;
  std::unordered_map<std::uint64_t, double> pending_hello_;       // transport id -> accept time
  std::unordered_map<std::uint64_t, std::uint64_t> attachments_;  // transport id -> token
  std::unordered_map<std::uint64_t, LogicalSession> sessions_;    // token -> session
  std::unordered_map<std::uint64_t, std::uint64_t> udp_index_;    // addr key -> token
  std::vector<std::uint64_t> dead_transports_;
  std::uint64_t next_transport_id_ = 1;
  Pcg32 token_rng_;

  // Simulation-thread state. journal_ is written by the sim thread only and
  // read via journal_text() after both threads are joined.
  sim::Journal journal_;

  // The two SPSC bridges (charter §4.6).
  SpscQueue<SimCommand> net_to_sim_{1024};
  SpscQueue<SimOutput> sim_to_net_{256};

  std::thread io_thread_;
  std::thread sim_thread_;
  std::atomic<bool> io_running_{false};
  std::atomic<bool> sim_running_{false};
  bool started_ = false;
  std::chrono::steady_clock::time_point start_time_;
};

}  // namespace seashield::server
