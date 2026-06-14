#include "client/core/client_session.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <span>

#include "protocol/reliable.h"

namespace seashield::client {

namespace {

using std::chrono::duration;
using std::chrono::steady_clock;

// Self-contained socket guard: clientcore must not drag net/ into the UE
// module, and the few calls needed here do not justify the dependency.
struct Fd {
  int fd = -1;
  ~Fd() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  bool valid() const { return fd >= 0; }
};

// 2-byte LE length-prefix framing, mirroring net::FrameParser's wire format
// (protocol-spec §2) with the same 16 KiB cap.
constexpr std::size_t kMaxFrameBytes = 16 * 1024;

void encode_frame(std::vector<std::uint8_t>& wire, const std::vector<std::uint8_t>& body) {
  wire.push_back(static_cast<std::uint8_t>(body.size() & 0xFF));
  wire.push_back(static_cast<std::uint8_t>((body.size() >> 8) & 0xFF));
  wire.insert(wire.end(), body.begin(), body.end());
}

class Framer {
 public:
  template <typename Callback>
  bool feed(std::span<const std::uint8_t> bytes, Callback&& on_frame) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    std::size_t cursor = 0;
    while (buffer_.size() - cursor >= 2) {
      const std::size_t length = static_cast<std::size_t>(buffer_[cursor]) |
                                 (static_cast<std::size_t>(buffer_[cursor + 1]) << 8);
      if (length > kMaxFrameBytes) {
        return false;
      }
      if (buffer_.size() - cursor - 2 < length) {
        break;
      }
      on_frame(std::span<const std::uint8_t>(buffer_.data() + cursor + 2, length));
      cursor += 2 + length;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(cursor));
    return true;
  }

 private:
  std::vector<std::uint8_t> buffer_;
};

bool send_all(int fd, const std::vector<std::uint8_t>& bytes) {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
#ifdef MSG_NOSIGNAL
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;  // macOS: SIGPIPE is ignored process-wide.
#endif
    const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, kFlags);
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
  encode_frame(wire, body);
  return send_all(fd, wire);
}

int connect_socket(int type, const std::string& host, std::uint16_t port) {
  const int fd = ::socket(AF_INET, type, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
      ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

void ClientSession::request_fire(const protocol::FireRequest& fire) {
  const std::lock_guard<std::mutex> lock(fire_mutex_);
  pending_fire_.push_back(fire);
}

void ClientSession::request_steer(const protocol::ShipCommand& steer) {
  const std::lock_guard<std::mutex> lock(fire_mutex_);
  pending_steer_.push_back(steer);
}

bool ClientSession::run() {
  const auto started = steady_clock::now();
  const auto now_s = [&] { return duration<double>(steady_clock::now() - started).count(); };
  const auto fail = [&](const std::string& what) {
    if (callbacks_.on_error) {
      callbacks_.on_error(what);
    }
    return false;
  };

  // --- Phase 1: TCP handshake ---
  Fd tcp{connect_socket(SOCK_STREAM, config_.host, config_.tcp_port)};
  if (!tcp.valid()) {
    return fail("tcp connect failed");
  }
  protocol::ClientHello hello;
  hello.role = config_.role;
  hello.token = config_.token;
  if (!send_control_frame(tcp.fd, protocol::encode_control_frame(hello))) {
    return fail("hello send failed");
  }

  timeval recv_timeout{5, 0};
  ::setsockopt(tcp.fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
  Framer tcp_framer;
  protocol::ServerWelcome welcome;
  bool have_welcome = false;
  bool rejected = false;
  std::uint8_t buf[4096];
  while (!have_welcome && !rejected && !stop_.load()) {
    const ssize_t n = ::recv(tcp.fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      return fail("tcp closed during handshake");
    }
    const bool ok = tcp_framer.feed(
        {buf, static_cast<std::size_t>(n)}, [&](std::span<const std::uint8_t> frame) {
          const auto message = protocol::decode_control_frame(frame);
          if (!message) {
            return;
          }
          if (const auto* w = std::get_if<protocol::ServerWelcome>(&*message)) {
            welcome = *w;
            have_welcome = true;
          } else if (const auto* r = std::get_if<protocol::ServerReject>(&*message)) {
            rejected = true;
            if (callbacks_.on_reject) {
              callbacks_.on_reject(r->reason);
            }
          }
        });
    if (!ok) {
      return fail("tcp framing violation");
    }
  }
  if (rejected || stop_.load()) {
    return false;
  }
  if (callbacks_.on_welcome) {
    callbacks_.on_welcome(welcome);
  }

  // --- Phase 2: UDP bind (hello until the ack lands) ---
  const std::uint16_t udp_port = config_.udp_port != 0 ? config_.udp_port : welcome.udp_port;
  Fd udp{connect_socket(SOCK_DGRAM, config_.host, udp_port)};
  if (!udp.valid()) {
    return fail("udp connect failed");
  }
  ::fcntl(udp.fd, F_SETFL, O_NONBLOCK);

  protocol::ReliableEndpoint endpoint;
  const auto flush_udp = [&] {
    endpoint.flush(now_s(), [&](std::span<const std::uint8_t> datagram) {
      (void)::send(udp.fd, datagram.data(), datagram.size(), 0);
    });
  };

  bool udp_bound = false;
  const auto on_message = [&](protocol::MsgType type, std::span<const std::uint8_t> payload) {
    const auto message = protocol::decode_data_message(type, payload);
    if (!message) {
      return;
    }
    if (std::holds_alternative<protocol::UdpHelloAck>(*message)) {
      udp_bound = true;
    } else if (const auto* snap = std::get_if<protocol::Snapshot>(&*message)) {
      if (callbacks_.on_snapshot) {
        callbacks_.on_snapshot(*snap);
      }
    } else if (const auto* delta = std::get_if<protocol::SnapshotDelta>(&*message)) {
      if (callbacks_.on_snapshot_delta) {
        callbacks_.on_snapshot_delta(*delta);
      }
    } else if (const auto* event = std::get_if<protocol::EngagementEvent>(&*message)) {
      if (callbacks_.on_event) {
        callbacks_.on_event(*event);
      }
    } else if (const auto* solution = std::get_if<protocol::FireSolution>(&*message)) {
      if (callbacks_.on_fire_solution) {
        callbacks_.on_fire_solution(*solution);
      }
    }
  };
  const auto drain_udp = [&] {
    std::uint8_t datagram[2048];
    for (;;) {
      const ssize_t n = ::recv(udp.fd, datagram, sizeof(datagram), 0);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return;  // EAGAIN: drained.
      }
      endpoint.on_datagram(now_s(), {datagram, static_cast<std::size_t>(n)}, on_message);
    }
  };

  const auto hello_payload =
      protocol::encode_payload(protocol::UdpHello{welcome.token, welcome.udp_nonce});
  const double bind_started = now_s();
  double next_hello = bind_started;
  while (!udp_bound && !stop_.load()) {
    if (now_s() - bind_started > config_.udp_hello_timeout_s) {
      return fail("udp bind timeout");
    }
    if (now_s() >= next_hello) {
      endpoint.send_unreliable(protocol::MsgType::kUdpHello, hello_payload);
      flush_udp();
      next_hello = now_s() + config_.udp_hello_interval_s;
    }
    pollfd pfd{udp.fd, POLLIN, 0};
    ::poll(&pfd, 1, 20);
    if ((pfd.revents & POLLIN) != 0) {
      drain_udp();
    }
  }

  // --- Phase 3: consume until told to stop ---
  double next_keepalive = now_s();
  while (!stop_.load()) {
    const double now = now_s();
    if (now >= next_keepalive) {
      endpoint.send_unreliable(protocol::MsgType::kKeepalive, {});
      next_keepalive = now + config_.keepalive_interval_s;
    }
    const std::int64_t ack_tick = pending_ack_.exchange(-1);
    if (ack_tick >= 0) {
      protocol::SnapshotAck ack;
      ack.tick = static_cast<std::uint32_t>(ack_tick);
      endpoint.send_unreliable(protocol::MsgType::kSnapshotAck, protocol::encode_payload(ack));
    }
    std::vector<protocol::FireRequest> fire_requests;
    std::vector<protocol::ShipCommand> steer_requests;
    {
      const std::lock_guard<std::mutex> lock(fire_mutex_);
      fire_requests.swap(pending_fire_);
      steer_requests.swap(pending_steer_);
    }
    for (const protocol::FireRequest& fire : fire_requests) {
      if (!send_control_frame(tcp.fd, protocol::encode_control_frame(fire))) {
        return fail("fire send failed");
      }
    }
    for (const protocol::ShipCommand& steer : steer_requests) {
      if (!send_control_frame(tcp.fd, protocol::encode_control_frame(steer))) {
        return fail("steer send failed");
      }
    }
    flush_udp();

    pollfd pfds[2] = {{udp.fd, POLLIN, 0}, {tcp.fd, POLLIN, 0}};
    ::poll(pfds, 2, 10);
    if ((pfds[0].revents & POLLIN) != 0) {
      drain_udp();
    }
    if ((pfds[1].revents & (POLLIN | POLLHUP)) != 0) {
      const ssize_t n = ::recv(tcp.fd, buf, sizeof(buf), 0);
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        return fail("server closed the session");
      }
      if (n > 0) {
        // Post-welcome TCP carries the bind-time event backlog (v4). Boundary
        // overlap with live UDP events is possible by design — the consumer
        // dedups on (kind, subject, tick).
        const bool ok = tcp_framer.feed(
            {buf, static_cast<std::size_t>(n)}, [&](std::span<const std::uint8_t> frame) {
              const auto message = protocol::decode_control_frame(frame);
              if (!message) {
                return;
              }
              if (const auto* backlog = std::get_if<protocol::EventBacklog>(&*message)) {
                for (const protocol::EngagementEvent& event : backlog->events) {
                  if (callbacks_.on_event) {
                    callbacks_.on_event(event);
                  }
                }
              }
            });
        if (!ok) {
          return fail("tcp framing violation");
        }
      }
    }
  }
  flush_udp();  // Final ack pass so the server's in-flight events drain.
  return true;
}

}  // namespace seashield::client
