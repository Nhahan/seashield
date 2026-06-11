#pragma once

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/pcg32.h"
#include "core/unique_fd.h"

namespace seashield::tools {

// Degraded-network injection settings (charter §10.3): applied independently
// to every datagram in both directions. Reordering is not a separate knob —
// jitter makes delivery times non-monotonic, which is how real networks
// reorder. All randomness comes from one seeded PCG32, so a given (seed,
// traffic) pair reproduces the same chaos.
struct ChaosConfig {
  std::uint16_t listen_port = 0;  // 0 = ephemeral.
  std::string upstream_host = "127.0.0.1";
  std::uint16_t upstream_port = 0;
  double loss = 0.0;      // P(drop) per datagram.
  double dup = 0.0;       // P(deliver twice) per surviving datagram.
  double delay_s = 0.0;   // Base one-way delay added.
  double jitter_s = 0.0;  // Uniform extra delay in [0, jitter_s).
  std::uint64_t seed = 1;
  std::size_t max_clients = 64;
};

// UDP chaos proxy: clients talk to the listen port, the proxy relays to the
// upstream server through one ephemeral socket per client (so return traffic
// finds its way back), injecting loss/duplication/latency on the way.
//
// Built in-house instead of dnctl/pfctl or tc-netem because the tests need
// the SAME degradation, seeded, on macOS and Linux (charter §10.3). Pure
// poll(2) loop, single thread; run() blocks until stop() — tests run it on a
// dedicated thread, the CLI runs it on main.
class ChaosProxy {
 public:
  explicit ChaosProxy(const ChaosConfig& config);
  ~ChaosProxy();

  ChaosProxy(const ChaosProxy&) = delete;
  ChaosProxy& operator=(const ChaosProxy&) = delete;

  // Binds the listen socket and the stop pipe. False on socket failure.
  bool init();

  std::uint16_t listen_port() const { return bound_port_; }

  // Blocking relay loop; returns after stop().
  void run();

  // Thread-safe, idempotent.
  void stop();

  std::uint64_t forwarded() const { return forwarded_.load(); }
  std::uint64_t dropped() const { return dropped_.load(); }
  std::uint64_t duplicated() const { return duplicated_.load(); }

 private:
  struct Client {
    sockaddr_in addr{};
    UniqueFd upstream_fd;  // Connected to the upstream server.
  };

  // A datagram waiting out its injected delay.
  struct Scheduled {
    double due_s = 0.0;
    std::uint64_t order = 0;  // Tie-break: preserves FIFO at equal due times.
    int fd = -1;
    bool to_client = false;
    sockaddr_in client_addr{};  // Valid when to_client.
    std::vector<std::uint8_t> bytes;

    bool operator>(const Scheduled& o) const {
      return due_s != o.due_s ? due_s > o.due_s : order > o.order;
    }
  };

  double now_s() const;
  void schedule(int fd, bool to_client, const sockaddr_in& client_addr,
                std::vector<std::uint8_t> bytes);
  void flush_due();
  void on_listen_readable();
  void on_upstream_readable(std::uint64_t client_key);
  static std::uint64_t addr_key(const sockaddr_in& addr);

  ChaosConfig config_;
  Pcg32 rng_;
  UniqueFd listen_fd_;
  UniqueFd stop_read_, stop_write_;
  std::uint16_t bound_port_ = 0;
  sockaddr_in upstream_addr_{};
  std::unordered_map<std::uint64_t, Client> clients_;
  std::priority_queue<Scheduled, std::vector<Scheduled>, std::greater<>> pending_;
  std::uint64_t next_order_ = 0;
  std::atomic<bool> stopping_{false};
  std::atomic<std::uint64_t> forwarded_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> duplicated_{0};
};

}  // namespace seashield::tools
