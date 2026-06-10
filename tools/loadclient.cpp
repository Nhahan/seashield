// Multi-connection demo/load client for the SeaShield P1 server.
//
//   loadclient [--host 127.0.0.1] [--port 7777] [--clients 8] [--messages 100]
//              [--payload 64] [--slow N] [--interval-ms 2]
//
// Normal clients send framed messages and count what comes back (echo or
// broadcast). The first N "--slow" clients connect with a tiny receive buffer
// and never read, so under broadcast load the server's per-session send cap
// trips and evicts them — without disturbing the other clients (charter §4.8).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/unique_fd.h"
#include "net/frame_parser.h"
#include "net/socket_util.h"

namespace {

using namespace std::chrono_literals;

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 7777;
  int clients = 8;
  int messages = 100;
  std::size_t payload = 64;
  int slow = 0;
  int interval_ms = 2;
};

struct ClientResult {
  int id = 0;
  bool slow = false;
  std::size_t sent = 0;
  std::size_t received = 0;
  bool disconnected = false;
};

bool parse_args(int argc, char** argv, Options& opts) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next_long = [&](long min, long max) -> long {
      if (i + 1 >= argc) {
        return min - 1;
      }
      const long value = std::strtol(argv[++i], nullptr, 10);
      return (value < min || value > max) ? min - 1 : value;
    };
    if (arg == "--host") {
      if (i + 1 >= argc) return false;
      opts.host = argv[++i];
    } else if (arg == "--port") {
      const long v = next_long(1, 65535);
      if (v < 1) return false;
      opts.port = static_cast<std::uint16_t>(v);
    } else if (arg == "--clients") {
      const long v = next_long(1, 1024);
      if (v < 1) return false;
      opts.clients = static_cast<int>(v);
    } else if (arg == "--messages") {
      const long v = next_long(1, 1000000);
      if (v < 1) return false;
      opts.messages = static_cast<int>(v);
    } else if (arg == "--payload") {
      const long v = next_long(8, 16384);
      if (v < 8) return false;
      opts.payload = static_cast<std::size_t>(v);
    } else if (arg == "--slow") {
      const long v = next_long(0, 1024);
      if (v < 0) return false;
      opts.slow = static_cast<int>(v);
    } else if (arg == "--interval-ms") {
      const long v = next_long(0, 1000);
      if (v < 0) return false;
      opts.interval_ms = static_cast<int>(v);
    } else {
      return false;
    }
  }
  return true;
}

seashield::UniqueFd connect_to(const Options& opts, bool slow) {
  seashield::UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!fd.valid()) {
    return {};
  }
  if (slow) {
    // A tiny receive window makes the server's queue back up quickly, so the
    // eviction demo does not need to push hundreds of megabytes first.
    const int tiny = 4096;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF, &tiny, sizeof(tiny));
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(opts.port);
  if (::inet_pton(AF_INET, opts.host.c_str(), &addr.sin_addr) != 1) {
    return {};
  }
  if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    return {};
  }
  seashield::net::set_nosigpipe(fd.get());
  return fd;
}

bool send_all(int fd, const std::vector<std::uint8_t>& bytes) {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent,
                             seashield::net::send_flags_nosignal());
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

// Reads whatever is available without blocking; returns false on disconnect.
bool drain_nonblocking(int fd, seashield::net::FrameParser& parser, std::size_t& received) {
  std::uint8_t buf[4096];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n > 0) {
      parser.feed({buf, static_cast<std::size_t>(n)},
                  [&](std::span<const std::uint8_t>) { ++received; });
      continue;
    }
    if (n == 0) {
      return false;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    }
    if (errno == EINTR) {
      continue;
    }
    return false;
  }
}

ClientResult run_client(const Options& opts, int id) {
  ClientResult result;
  result.id = id;
  result.slow = id < opts.slow;

  seashield::UniqueFd fd = connect_to(opts, result.slow);
  if (!fd.valid()) {
    std::fprintf(stderr, "client %d: connect failed (errno=%d)\n", id, errno);
    result.disconnected = true;
    return result;
  }

  seashield::net::FrameParser parser;
  for (int m = 0; m < opts.messages; ++m) {
    char tag[32];
    std::snprintf(tag, sizeof(tag), "c%03d-m%05d", id, m);
    std::string payload(tag);
    payload.resize(opts.payload, '.');

    std::vector<std::uint8_t> wire;
    seashield::net::FrameParser::encode(
        wire, {reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()});
    if (!send_all(fd.get(), wire)) {
      result.disconnected = true;  // Evicted or server gone.
      return result;
    }
    ++result.sent;

    if (!result.slow && !drain_nonblocking(fd.get(), parser, result.received)) {
      result.disconnected = true;
      return result;
    }
    if (opts.interval_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(opts.interval_ms));
    }
  }

  if (result.slow) {
    // Hold the connection without reading; detect eviction via recv.
    timeval timeout{5, 0};
    ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char probe;
    const ssize_t n = ::recv(fd.get(), &probe, 1, 0);
    result.disconnected = (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK));
    return result;
  }

  // Final drain window for in-flight broadcasts.
  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!drain_nonblocking(fd.get(), parser, result.received)) {
      result.disconnected = true;
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!parse_args(argc, argv, opts)) {
    std::fprintf(stderr,
                 "usage: %s [--host H] [--port N] [--clients N] [--messages N] "
                 "[--payload BYTES] [--slow N] [--interval-ms N]\n",
                 argv[0]);
    return 2;
  }
  seashield::net::ignore_sigpipe();

  std::vector<std::thread> threads;
  std::vector<ClientResult> results(static_cast<std::size_t>(opts.clients));
  threads.reserve(static_cast<std::size_t>(opts.clients));
  for (int i = 0; i < opts.clients; ++i) {
    threads.emplace_back(
        [&opts, &results, i] { results[static_cast<std::size_t>(i)] = run_client(opts, i); });
  }
  for (auto& t : threads) {
    t.join();
  }

  std::size_t evicted = 0;
  bool normal_failure = false;
  std::printf("\n%-8s %-6s %-10s %-10s %s\n", "client", "mode", "sent", "received", "status");
  for (const auto& r : results) {
    const bool eviction = r.slow && r.disconnected;
    evicted += eviction ? 1u : 0u;
    if (!r.slow && (r.disconnected || r.sent != static_cast<std::size_t>(opts.messages))) {
      normal_failure = true;
    }
    std::printf("%-8d %-6s %-10zu %-10zu %s\n", r.id, r.slow ? "slow" : "normal", r.sent,
                r.received, eviction          ? "evicted (expected)"
                            : r.disconnected ? "DISCONNECTED"
                                             : "ok");
  }
  std::printf("\nsummary: clients=%d slow=%d evicted=%zu\n", opts.clients, opts.slow, evicted);

  if (normal_failure) {
    std::printf("FAIL: a normal client lost its connection or could not finish sending\n");
    return 1;
  }
  if (opts.slow > 0 && evicted == 0) {
    std::printf("WARN: no slow client was evicted (cap not reached — raise --messages/--payload)\n");
  }
  return 0;
}
