#include "tools/dummy_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <set>
#include <tuple>
#include <variant>

#include "core/unique_fd.h"
#include "net/frame_parser.h"
#include "net/socket_util.h"
#include "protocol/reliable.h"

namespace seashield::tools {
namespace {

using namespace std::chrono;

UniqueFd connect_tcp(const std::string& host, std::uint16_t port) {
  UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!fd.valid()) {
    return {};
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
      ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    return {};
  }
  net::set_nosigpipe(fd.get());
  return fd;
}

UniqueFd connect_udp(const std::string& host, std::uint16_t port) {
  UniqueFd fd(::socket(AF_INET, SOCK_DGRAM, 0));
  if (!fd.valid()) {
    return {};
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
      ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    return {};
  }
  return fd;
}

bool send_all(int fd, std::span<const std::uint8_t> bytes) {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const ssize_t n =
        ::send(fd, bytes.data() + sent, bytes.size() - sent, net::send_flags_nosignal());
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

bool send_control_frame(int fd, const std::vector<std::uint8_t>& body) {
  std::vector<std::uint8_t> wire;
  net::FrameParser::encode(wire, body);
  return send_all(fd, wire);
}

}  // namespace

DummyClientReport DummyClient::run() {
  DummyClientReport report;
  const auto started = steady_clock::now();
  const auto now_s = [&] { return duration<double>(steady_clock::now() - started).count(); };

  // --- Phase 1: TCP handshake (charter §6 세션 흐름) ---
  UniqueFd tcp = connect_tcp(config_.host, config_.tcp_port);
  if (!tcp.valid()) {
    report.error = "tcp connect failed";
    return report;
  }
  report.connected = true;

  protocol::ClientHello hello;
  hello.role = config_.role;
  hello.token = config_.token;
  if (!send_control_frame(tcp.get(), protocol::encode_control_frame(hello))) {
    report.error = "hello send failed";
    return report;
  }

  timeval recv_timeout{5, 0};
  ::setsockopt(tcp.get(), SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
  net::FrameParser tcp_parser;
  protocol::ServerWelcome welcome;
  bool have_welcome = false;
  std::uint8_t buf[4096];
  while (!have_welcome && !report.rejected) {
    const ssize_t n = ::recv(tcp.get(), buf, sizeof(buf), 0);
    if (n <= 0) {
      report.error = "tcp closed during handshake";
      return report;
    }
    const bool ok = tcp_parser.feed(
        {buf, static_cast<std::size_t>(n)}, [&](std::span<const std::uint8_t> frame) {
          const auto message = protocol::decode_control_frame(frame);
          if (!message) {
            return;
          }
          if (const auto* w = std::get_if<protocol::ServerWelcome>(&*message)) {
            welcome = *w;
            have_welcome = true;
          } else if (const auto* r = std::get_if<protocol::ServerReject>(&*message)) {
            report.rejected = true;
            report.reject_reason = r->reason;
          }
        });
    if (!ok) {
      report.error = "tcp framing violation";
      return report;
    }
  }
  if (report.rejected) {
    return report;
  }
  report.welcomed = true;
  report.token = welcome.token;
  report.role = welcome.role;
  report.weather_summary = welcome.weather_summary;

  // --- Phase 2: UDP bind — repeat the hello until the ack lands ---
  const std::uint16_t udp_port = config_.udp_port != 0 ? config_.udp_port : welcome.udp_port;
  UniqueFd udp = connect_udp(config_.host, udp_port);
  if (!udp.valid()) {
    report.error = "udp socket failed";
    return report;
  }
  if (!net::set_nonblocking_cloexec(udp.get()) || !net::set_nonblocking_cloexec(tcp.get())) {
    report.error = "nonblocking setup failed";
    return report;
  }

  protocol::ReliableEndpoint endpoint;
  const auto flush_udp = [&] {
    endpoint.flush(now_s(), [&](std::span<const std::uint8_t> datagram) {
      [[maybe_unused]] const ssize_t n = ::send(udp.get(), datagram.data(), datagram.size(), 0);
    });
  };

  std::set<std::tuple<std::uint8_t, std::uint16_t, std::uint32_t>> seen_events;
  const auto on_message = [&](protocol::MsgType type, std::span<const std::uint8_t> payload) {
    const auto message = protocol::decode_data_message(type, payload);
    if (!message) {
      return;
    }
    if (std::holds_alternative<protocol::UdpHelloAck>(*message)) {
      report.udp_bound = true;
    } else if (const auto* snap = std::get_if<protocol::Snapshot>(&*message)) {
      ++report.snapshot_batches;
      if (report.snapshot_ticks == 0 || snap->tick != report.last_tick) {
        ++report.snapshot_ticks;
        report.last_tick = snap->tick;
      }
      report.last_total_entities = snap->total_entities;
      report.last_phase = snap->phase;
    } else if (const auto* event = std::get_if<protocol::EngagementEvent>(&*message)) {
      const auto key = std::make_tuple(static_cast<std::uint8_t>(event->kind), event->subject_id,
                                       event->tick);
      if (!seen_events.insert(key).second) {
        report.duplicate_event = true;
      }
      report.events.push_back(*event);
    }
  };

  const auto drain_udp = [&] {
    std::uint8_t datagram[2048];
    for (;;) {
      const ssize_t n = ::recv(udp.get(), datagram, sizeof(datagram), 0);
      if (n < 0) {
        if (errno == EINTR) continue;
        return;  // EAGAIN: drained.
      }
      endpoint.on_datagram(now_s(), {datagram, static_cast<std::size_t>(n)}, on_message);
    }
  };

  const auto hello_payload = protocol::encode_payload(protocol::UdpHello{report.token});
  const double bind_started = now_s();
  double next_hello = bind_started;
  while (!report.udp_bound && !stop_.load()) {
    if (now_s() - bind_started > config_.udp_hello_timeout_s) {
      report.error = "udp bind timeout";
      return report;
    }
    if (now_s() >= next_hello) {
      endpoint.send_unreliable(protocol::MsgType::kUdpHello, hello_payload);
      flush_udp();
      next_hello = now_s() + config_.udp_hello_interval_s;
    }
    pollfd pfd{udp.get(), POLLIN, 0};
    ::poll(&pfd, 1, 20);
    if ((pfd.revents & POLLIN) != 0) {
      drain_udp();
    }
  }

  // --- Phase 3: consume the engagement ---
  const double run_started = now_s();
  double next_keepalive = run_started;
  bool fired = false;
  while (!stop_.load() && now_s() - run_started < config_.duration_s) {
    const double now = now_s();
    if (now >= next_keepalive) {
      endpoint.send_unreliable(protocol::MsgType::kKeepalive, {});
      next_keepalive = now + config_.keepalive_interval_s;
    }
    if (!fired && config_.fire_after_s >= 0.0 && now - run_started >= config_.fire_after_s) {
      fired = true;
      if (!send_control_frame(tcp.get(), protocol::encode_control_frame(config_.fire))) {
        report.disconnected_early = true;
        break;
      }
    }
    flush_udp();

    pollfd pfds[2] = {{udp.get(), POLLIN, 0}, {tcp.get(), POLLIN, 0}};
    ::poll(pfds, 2, 10);
    if ((pfds[0].revents & POLLIN) != 0) {
      drain_udp();
    }
    if ((pfds[1].revents & (POLLIN | POLLHUP)) != 0) {
      const ssize_t n = ::recv(tcp.get(), buf, sizeof(buf), 0);
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        report.disconnected_early = true;
        break;
      }
      // The P3 server sends nothing further over TCP after the welcome;
      // anything readable is drained and ignored.
    }
  }
  flush_udp();  // Final ack pass so the server's in-flight events drain.
  return report;
}

}  // namespace seashield::tools
