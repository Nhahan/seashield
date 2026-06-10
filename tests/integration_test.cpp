#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "core/unique_fd.h"
#include "net/acceptor.h"
#include "net/event_loop.h"
#include "net/frame_parser.h"
#include "net/socket_util.h"
#include "net/tcp_session.h"
#include "net/udp_endpoint.h"

// Loopback integration tests. The event loop runs on a dedicated thread; the
// test body only touches blocking client sockets and atomics, honoring the
// thread-affinity contract (design doc §7/§10).
namespace seashield::net {
namespace {

using namespace std::chrono;
using namespace std::chrono_literals;

bool wait_for(const std::function<bool()>& predicate, milliseconds timeout) {
  const auto deadline = steady_clock::now() + timeout;
  while (steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

UniqueFd connect_blocking(std::uint16_t port) {
  UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!fd.valid()) {
    return {};
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    return {};
  }
  timeval timeout{5, 0};
  ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  set_nosigpipe(fd.get());
  return fd;
}

void send_all(int fd, std::span<const std::uint8_t> bytes) {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, send_flags_nosignal());
    ASSERT_GT(n, 0) << "send failed: errno=" << errno;
    sent += static_cast<std::size_t>(n);
  }
}

void send_frame(int fd, const std::string& payload) {
  std::vector<std::uint8_t> wire;
  FrameParser::encode(wire, {reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()});
  send_all(fd, wire);
}

std::vector<std::string> read_frames(int fd, std::size_t count) {
  std::vector<std::string> frames;
  FrameParser parser;
  std::uint8_t buf[4096];
  while (frames.size() < count) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      ADD_FAILURE() << "recv failed or closed: n=" << n << " errno=" << errno;
      return frames;
    }
    const bool ok = parser.feed({buf, static_cast<std::size_t>(n)},
                                [&](std::span<const std::uint8_t> frame) {
                                  frames.emplace_back(frame.begin(), frame.end());
                                });
    if (!ok) {
      ADD_FAILURE() << "protocol violation in echo stream";
      return frames;
    }
  }
  return frames;
}

class IntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ignore_sigpipe();
    loop_ = EventLoop::create();
    ASSERT_NE(loop_, nullptr);
  }

  void TearDown() override { stop(); }

  // Must be called before start(): registration happens on the test thread
  // while the loop is not yet running.
  void make_server(std::size_t send_cap, TcpSession::FrameHandler frame_handler) {
    frame_handler_ = std::move(frame_handler);
    acceptor_ = std::make_unique<Acceptor>(
        *loop_, [this, send_cap](UniqueFd fd, const sockaddr_in&) {
          const std::uint64_t id = next_id_++;
          auto session = std::make_unique<TcpSession>(*loop_, std::move(fd), id, send_cap);
          TcpSession* raw = session.get();
          sessions_[id] = std::move(session);
          raw->start(
              [this](TcpSession& s, std::span<const std::uint8_t> frame) {
                frame_handler_(s, frame);
              },
              [this](TcpSession& s, const char* reason) {
                if (std::string(reason).find("overflow") != std::string::npos) {
                  evicted_.fetch_add(1);
                }
                closed_.fetch_add(1);
                dead_.push_back(s.id());  // Deferred deletion (design §6.4).
              });
          connected_.fetch_add(1);
        });
    ASSERT_TRUE(acceptor_->listen(0));
    port_ = acceptor_->port();
    ASSERT_NE(port_, 0);
  }

  void make_udp_echo() {
    udp_ = std::make_unique<UdpEndpoint>(*loop_);
    ASSERT_TRUE(udp_->open(0, [this](std::span<const std::uint8_t> payload, const sockaddr_in& from) {
      udp_->send_to(payload, from);
    }));
    udp_port_ = udp_->port();
    ASSERT_NE(udp_port_, 0);
  }

  void start() {
    running_.store(true);
    loop_thread_ = std::thread([this] {
      while (running_.load()) {
        loop_->run_once(50);
        reap();
      }
      reap();
    });
  }

  void stop() {
    if (loop_thread_.joinable()) {
      running_.store(false);
      loop_->wakeup();
      loop_thread_.join();
    }
  }

  void reap() {
    for (const std::uint64_t id : dead_) {
      sessions_.erase(id);
    }
    if (!dead_.empty() && acceptor_ && acceptor_->paused()) {
      acceptor_->resume();
    }
    dead_.clear();
  }

  std::unique_ptr<EventLoop> loop_;
  std::unique_ptr<Acceptor> acceptor_;
  std::unique_ptr<UdpEndpoint> udp_;
  TcpSession::FrameHandler frame_handler_;
  std::unordered_map<std::uint64_t, std::unique_ptr<TcpSession>> sessions_;
  std::vector<std::uint64_t> dead_;
  std::uint64_t next_id_ = 1;
  std::uint16_t port_ = 0;
  std::uint16_t udp_port_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<int> connected_{0};
  std::atomic<int> closed_{0};
  std::atomic<int> evicted_{0};
  std::thread loop_thread_;
};

TEST_F(IntegrationTest, EchoRoundTripWithCoalescedAndSplitFrames) {
  make_server(256 * 1024,
              [](TcpSession& s, std::span<const std::uint8_t> frame) { s.send(frame); });
  start();

  UniqueFd client = connect_blocking(port_);
  ASSERT_TRUE(client.valid());

  // 2.5 frames in the first write, the remaining half in the second: the
  // stream may arrive arbitrarily coalesced/split and framing must hold.
  std::vector<std::uint8_t> wire;
  for (const char* payload : {"alpha", "bravo", "charlie"}) {
    FrameParser::encode(wire,
                        {reinterpret_cast<const std::uint8_t*>(payload), std::strlen(payload)});
  }
  const std::size_t split = wire.size() - 4;
  send_all(client.get(), {wire.data(), split});
  std::this_thread::sleep_for(50ms);  // Force the server to see a partial frame.
  send_all(client.get(), {wire.data() + split, wire.size() - split});

  const auto frames = read_frames(client.get(), 3);
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_EQ(frames[0], "alpha");
  EXPECT_EQ(frames[1], "bravo");
  EXPECT_EQ(frames[2], "charlie");
}

TEST_F(IntegrationTest, EightClientBroadcast) {
  make_server(256 * 1024, [this](TcpSession&, std::span<const std::uint8_t> frame) {
    for (auto& [id, session] : sessions_) {
      session->send(frame);
    }
  });
  start();

  constexpr int kClients = 8;
  std::vector<UniqueFd> clients;
  for (int i = 0; i < kClients; ++i) {
    clients.push_back(connect_blocking(port_));
    ASSERT_TRUE(clients.back().valid());
  }
  ASSERT_TRUE(wait_for([&] { return connected_.load() == kClients; }, 5s));

  for (int i = 0; i < kClients; ++i) {
    send_frame(clients[i].get(), "msg" + std::to_string(i));
  }

  std::set<std::string> expected;
  for (int i = 0; i < kClients; ++i) {
    expected.insert("msg" + std::to_string(i));
  }
  for (int i = 0; i < kClients; ++i) {
    const auto frames = read_frames(clients[i].get(), kClients);
    ASSERT_EQ(frames.size(), static_cast<std::size_t>(kClients)) << "client " << i;
    EXPECT_EQ(std::set<std::string>(frames.begin(), frames.end()), expected) << "client " << i;
  }
}

TEST_F(IntegrationTest, SlowClientIsEvictedWithoutHarmingOthers) {
  // 'B' triggers a 4 MiB burst at the sender; anything else echoes. The burst
  // overwhelms kernel buffers + the 8 KiB queue cap of a non-reading client.
  make_server(8 * 1024, [](TcpSession& s, std::span<const std::uint8_t> frame) {
    if (!frame.empty() && frame[0] == 'B') {
      const std::vector<std::uint8_t> big(1024, 0xCD);
      for (int i = 0; i < 4096; ++i) {
        if (!s.send(big)) {
          break;  // Evicted mid-burst.
        }
      }
    } else {
      s.send(frame);
    }
  });
  start();

  UniqueFd slow = connect_blocking(port_);
  UniqueFd healthy = connect_blocking(port_);
  ASSERT_TRUE(slow.valid());
  ASSERT_TRUE(healthy.valid());
  ASSERT_TRUE(wait_for([&] { return connected_.load() == 2; }, 5s));

  send_frame(slow.get(), "Burst");  // Never reads the response.
  ASSERT_TRUE(wait_for([&] { return evicted_.load() >= 1; }, 10s))
      << "slow client was not evicted";

  // Isolation (G4): the healthy client must be completely unaffected.
  send_frame(healthy.get(), "ping");
  const auto frames = read_frames(healthy.get(), 1);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0], "ping");
  EXPECT_EQ(evicted_.load(), 1);
}

TEST_F(IntegrationTest, UdpEchoRoundTrip) {
  make_udp_echo();
  start();

  UniqueFd client(::socket(AF_INET, SOCK_DGRAM, 0));
  ASSERT_TRUE(client.valid());
  timeval timeout{5, 0};
  ::setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  sockaddr_in server{};
  server.sin_family = AF_INET;
  server.sin_port = htons(udp_port_);
  server.sin_addr.s_addr = inet_addr("127.0.0.1");

  const std::string payload = "datagram";
  ASSERT_EQ(::sendto(client.get(), payload.data(), payload.size(), 0,
                     reinterpret_cast<const sockaddr*>(&server), sizeof(server)),
            static_cast<ssize_t>(payload.size()));

  char buf[256];
  const ssize_t n = ::recvfrom(client.get(), buf, sizeof(buf), 0, nullptr, nullptr);
  ASSERT_EQ(n, static_cast<ssize_t>(payload.size()));
  EXPECT_EQ(std::string(buf, static_cast<std::size_t>(n)), payload);
}

}  // namespace
}  // namespace seashield::net
