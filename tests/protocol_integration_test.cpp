#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "core/math.h"
#include "core/unique_fd.h"
#include "net/frame_parser.h"
#include "net/socket_util.h"
#include "protocol/messages.h"
#include "protocol/reliable.h"
#include "server/sim_server.h"
#include "sim/journal.h"
#include "sim/scenario.h"
#include "tools/dummy_client.h"
#include "tools/net_chaos_proxy.h"

// P3b end-to-end: real sockets on loopback, the full stack — SimServer (two
// threads + SPSC bridges) ↔ dummy clients — with and without the chaos proxy
// in the UDP path (charter §9 P3 DoD, §10.3). Ports are always ephemeral.
namespace seashield {
namespace {

using namespace std::chrono_literals;

// Calm, fully pinned weather (no generator surprises) and short-lived rockets:
// a low-elevation shot splashes within ~2s, so every salvo fully adjudicates
// inside the test window.
constexpr const char* kScenarioText = R"(
weather_seed = 7
sim_seed = 3
gust_seed = 5
duration_s = 30
target_x = 4000
target_y = 4000
target_z = 300
target_heading_deg = 225
target_speed = 240
temperature_c = 15
humidity = 0.5
rain = 0
turbulence = 0.05
surface_wind_speed = 4
surface_wind_from_deg = 270
rocket_lifetime = 6
)";

class ProtocolIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override { net::ignore_sigpipe(); }

  void start_server(const std::function<void(server::SimServerConfig&)>& tweak = {}) {
    std::string error;
    const auto scenario = sim::load_scenario_text(kScenarioText, &error);
    ASSERT_TRUE(scenario.has_value()) << error;
    server::SimServerConfig config;
    config.tcp_port = 0;
    config.udp_port = 0;
    config.scenario = *scenario;
    if (tweak) {
      tweak(config);
    }
    server_ = std::make_unique<server::SimServer>(config);
    ASSERT_TRUE(server_->start());
  }

  // Runs every client concurrently (their own threads) and returns the
  // reports in input order.
  std::vector<tools::DummyClientReport> run_clients(
      const std::vector<tools::DummyClientConfig>& configs) {
    std::vector<tools::DummyClientReport> reports(configs.size());
    std::vector<std::thread> threads;
    threads.reserve(configs.size());
    for (std::size_t i = 0; i < configs.size(); ++i) {
      threads.emplace_back([this, &configs, &reports, i] {
        tools::DummyClient client(with_ports(configs[i]));
        reports[i] = client.run();
      });
    }
    for (auto& t : threads) {
      t.join();
    }
    return reports;
  }

  tools::DummyClientConfig with_ports(tools::DummyClientConfig config) const {
    config.tcp_port = server_->tcp_port();
    return config;
  }

  static tools::DummyClientConfig low_splash_fire(double duration_s) {
    tools::DummyClientConfig config;
    config.role = protocol::Role::kWeapons;
    config.duration_s = duration_s;
    config.fire_after_s = 0.3;
    config.fire.azimuth_rad = math::deg_to_rad(45.0);
    config.fire.elevation_rad = math::deg_to_rad(1.0);  // Splashes in ~2s.
    config.fire.salvo_count = 4;
    config.fire.dispersion_mrad = 3.0;
    config.fire.launch_interval_s = 0.05;
    return config;
  }

  static std::multiset<std::tuple<std::uint8_t, std::uint16_t>> event_multiset(
      const tools::DummyClientReport& report) {
    std::multiset<std::tuple<std::uint8_t, std::uint16_t>> set;
    for (const auto& event : report.events) {
      set.emplace(static_cast<std::uint8_t>(event.kind), event.subject_id);
    }
    return set;
  }

  static std::size_t count_kind(const tools::DummyClientReport& report, protocol::EventKind kind) {
    return static_cast<std::size_t>(
        std::count_if(report.events.begin(), report.events.end(),
                      [kind](const protocol::EngagementEvent& e) { return e.kind == kind; }));
  }

  std::unique_ptr<server::SimServer> server_;
};

TEST_F(ProtocolIntegrationTest, SnapshotsFlowToEveryRole) {
  start_server();
  tools::DummyClientConfig weapons;
  weapons.role = protocol::Role::kWeapons;
  weapons.duration_s = 1.5;
  tools::DummyClientConfig commander = weapons;
  commander.role = protocol::Role::kCommander;
  tools::DummyClientConfig observer = weapons;
  observer.role = protocol::Role::kObserver;

  const auto reports = run_clients({weapons, commander, observer});
  ASSERT_EQ(reports.size(), 3u);
  EXPECT_EQ(reports[0].role, protocol::Role::kWeapons);
  EXPECT_EQ(reports[1].role, protocol::Role::kCommander);
  EXPECT_EQ(reports[2].role, protocol::Role::kObserver);
  for (const auto& report : reports) {
    EXPECT_TRUE(report.welcomed) << report.error;
    EXPECT_TRUE(report.udp_bound) << report.error;
    EXPECT_FALSE(report.disconnected_early);
    // 1.5s at 30Hz is ~45 distinct ticks; demand a conservative majority.
    EXPECT_GE(report.snapshot_ticks, 20u);
    EXPECT_EQ(report.last_total_entities, 1u);  // Target only before any fire.
    EXPECT_FALSE(report.weather_summary.empty());
    EXPECT_EQ(report.last_phase, protocol::EngagementPhase::kRunning);
  }
  EXPECT_EQ(server_->stats().sessions_created.load(), 3u);
}

TEST_F(ProtocolIntegrationTest, ExclusiveRolesAreRejectedWhileSeatIsLive) {
  start_server();
  tools::DummyClientConfig first_weapons;
  first_weapons.role = protocol::Role::kWeapons;
  first_weapons.duration_s = 1.5;

  std::thread holder([&] {
    tools::DummyClient client(with_ports(first_weapons));
    const auto report = client.run();
    EXPECT_TRUE(report.welcomed) << report.error;
  });
  std::this_thread::sleep_for(400ms);  // Let the first client claim the seat.

  tools::DummyClientConfig second_weapons = first_weapons;
  second_weapons.duration_s = 0.3;
  tools::DummyClient rejected_client(with_ports(second_weapons));
  const auto rejected = rejected_client.run();
  EXPECT_TRUE(rejected.rejected);
  EXPECT_EQ(rejected.reject_reason, protocol::RejectReason::kRoleTaken);

  tools::DummyClientConfig observer = first_weapons;
  observer.role = protocol::Role::kObserver;
  observer.duration_s = 0.3;
  tools::DummyClient observer_client(with_ports(observer));
  const auto welcomed = observer_client.run();
  EXPECT_TRUE(welcomed.welcomed) << welcomed.error;

  holder.join();
}

TEST_F(ProtocolIntegrationTest, SalvoEventsArriveExactlyOnceForEveryConsole) {
  start_server();
  auto weapons = low_splash_fire(4.0);
  tools::DummyClientConfig observer;
  observer.role = protocol::Role::kObserver;
  observer.duration_s = 4.0;

  const auto reports = run_clients({weapons, observer});
  for (const auto& report : reports) {
    ASSERT_TRUE(report.welcomed) << report.error;
    EXPECT_FALSE(report.duplicate_event);
    EXPECT_EQ(count_kind(report, protocol::EventKind::kLaunch), 4u);
    EXPECT_EQ(count_kind(report, protocol::EventKind::kRocketResolved), 4u);
    // Rockets appeared in snapshots while in flight, then resolved out.
    EXPECT_GT(report.snapshot_ticks, 60u);
  }
  // Both consoles saw the SAME engagement.
  EXPECT_EQ(event_multiset(reports[0]), event_multiset(reports[1]));

  // The operator input crossed the SPSC bridge and was journaled with its
  // tick — the replay artifact of charter §5.8.
  EXPECT_EQ(server_->stats().commands_accepted.load(), 1u);
  server_->stop();
  const auto journal = sim::Journal::parse(server_->journal_text());
  ASSERT_TRUE(journal.has_value());
  ASSERT_EQ(journal->entries().size(), 1u);
  EXPECT_EQ(journal->entries()[0].command.salvo_count, 4);
  EXPECT_DOUBLE_EQ(journal->entries()[0].command.azimuth_rad, math::deg_to_rad(45.0));
}

TEST_F(ProtocolIntegrationTest, TokenReconnectRestoresRoleAndBadTokenIsRejected) {
  start_server();

  tools::DummyClientConfig weapons;
  weapons.role = protocol::Role::kWeapons;
  weapons.duration_s = 0.8;
  tools::DummyClient first(with_ports(weapons));
  const auto first_report = first.run();
  ASSERT_TRUE(first_report.welcomed) << first_report.error;
  ASSERT_NE(first_report.token, 0u);
  std::this_thread::sleep_for(200ms);  // Transport reaped; seat now reserved, not live.

  // While detached, the seat is held by the token: a fresh weapons hello
  // (no token) must still be refused.
  tools::DummyClientConfig usurper = weapons;
  usurper.duration_s = 0.3;
  tools::DummyClient usurper_client(with_ports(usurper));
  const auto usurped = usurper_client.run();
  EXPECT_TRUE(usurped.rejected);
  EXPECT_EQ(usurped.reject_reason, protocol::RejectReason::kRoleTaken);

  // Reconnect with the token: requested role is ignored, the reserved seat
  // comes back (charter §4.8 재접속 복귀) and the channel works end-to-end.
  auto reconnect = low_splash_fire(4.0);
  reconnect.role = protocol::Role::kObserver;
  reconnect.token = first_report.token;
  tools::DummyClient second(with_ports(reconnect));
  const auto second_report = second.run();
  ASSERT_TRUE(second_report.welcomed) << second_report.error;
  EXPECT_EQ(second_report.token, first_report.token);
  EXPECT_EQ(second_report.role, protocol::Role::kWeapons);
  EXPECT_TRUE(second_report.udp_bound);
  EXPECT_EQ(count_kind(second_report, protocol::EventKind::kRocketResolved), 4u);
  EXPECT_EQ(server_->stats().sessions_reattached.load(), 1u);

  tools::DummyClientConfig bogus;
  bogus.token = 0xDEADBEEFDEADBEEFull;
  bogus.duration_s = 0.3;
  tools::DummyClient bogus_client(with_ports(bogus));
  const auto bogus_report = bogus_client.run();
  EXPECT_TRUE(bogus_report.rejected);
  EXPECT_EQ(bogus_report.reject_reason, protocol::RejectReason::kBadToken);
}

// A console that binds UDP and then never acknowledges anything (process
// frozen, cable pulled) must lose its binding via the reliable-channel peer
// timeout — and, mirroring the P1 slow-client test, without disturbing the
// healthy console (charter §4.8 격리 원칙, UDP edition).
TEST_F(ProtocolIntegrationTest, SilentClientLosesUdpBindingWithoutHarmingOthers) {
  start_server([](server::SimServerConfig& config) { config.reliable_peer_timeout_s = 1.2; });

  // Hand-rolled minimal client: TCP hello -> welcome -> UDP hello, then
  // total silence (DummyClient is too polite — it acks).
  UniqueFd tcp(::socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_TRUE(tcp.valid());
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(server_->tcp_port());
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  ASSERT_EQ(::connect(tcp.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);
  net::set_nosigpipe(tcp.get());

  std::vector<std::uint8_t> frame;
  net::FrameParser::encode(frame, protocol::encode_control_frame(protocol::ClientHello{}));
  ASSERT_EQ(::send(tcp.get(), frame.data(), frame.size(), net::send_flags_nosignal()),
            static_cast<ssize_t>(frame.size()));

  std::uint64_t token = 0;
  net::FrameParser parser;
  std::uint8_t buf[2048];
  while (token == 0) {
    const ssize_t n = ::recv(tcp.get(), buf, sizeof(buf), 0);
    ASSERT_GT(n, 0);
    ASSERT_TRUE(parser.feed({buf, static_cast<std::size_t>(n)},
                            [&](std::span<const std::uint8_t> body) {
                              const auto message = protocol::decode_control_frame(body);
                              ASSERT_TRUE(message.has_value());
                              token = std::get<protocol::ServerWelcome>(*message).token;
                            }));
  }

  UniqueFd udp(::socket(AF_INET, SOCK_DGRAM, 0));
  ASSERT_TRUE(udp.valid());
  addr.sin_port = htons(server_->udp_port());
  ASSERT_EQ(::connect(udp.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);
  protocol::ReliableEndpoint endpoint;
  const auto hello = protocol::encode_payload(protocol::UdpHello{token});
  timeval timeout{0, 200 * 1000};
  ::setsockopt(udp.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  bool bound = false;
  for (int attempt = 0; attempt < 25 && !bound; ++attempt) {
    endpoint.send_unreliable(protocol::MsgType::kUdpHello, hello);
    endpoint.flush(0.1 * attempt, [&](std::span<const std::uint8_t> datagram) {
      [[maybe_unused]] const ssize_t sent =
          ::send(udp.get(), datagram.data(), datagram.size(), 0);
    });
    bound = ::recv(udp.get(), buf, sizeof(buf), 0) > 0;  // Server talks only to bound peers.
  }
  ASSERT_TRUE(bound);
  // From here on: complete silence. Events pile up unacknowledged.

  auto weapons = low_splash_fire(4.0);
  tools::DummyClient healthy(with_ports(weapons));
  const auto report = healthy.run();
  ASSERT_TRUE(report.welcomed) << report.error;
  EXPECT_FALSE(report.duplicate_event);
  EXPECT_EQ(count_kind(report, protocol::EventKind::kLaunch), 4u);
  EXPECT_EQ(count_kind(report, protocol::EventKind::kRocketResolved), 4u);
  EXPECT_GE(server_->stats().udp_unbound_timeouts.load(), 1u)
      << "silent client kept its UDP binding past the peer timeout";
}

// The P3a gate, now over REAL sockets: 10% loss + duplication + 30ms jitter
// (reordering) injected between client and server. Snapshots keep flowing
// (loss-tolerant by design) and reliable events still arrive exactly once.
TEST_F(ProtocolIntegrationTest, ChaosProxyLossAndReorderDoNotBreakTheProtocol) {
  start_server();

  tools::ChaosConfig chaos;
  chaos.upstream_port = server_->udp_port();
  chaos.loss = 0.10;
  chaos.dup = 0.05;
  chaos.delay_s = 0.010;
  chaos.jitter_s = 0.030;
  chaos.seed = 42;
  tools::ChaosProxy proxy(chaos);
  ASSERT_TRUE(proxy.init());
  std::thread proxy_thread([&] { proxy.run(); });

  auto weapons = low_splash_fire(5.0);
  weapons.udp_port = proxy.listen_port();  // UDP detours through the chaos.
  tools::DummyClientConfig observer;
  observer.role = protocol::Role::kObserver;
  observer.duration_s = 5.0;
  observer.udp_port = proxy.listen_port();

  const auto reports = run_clients({weapons, observer});
  proxy.stop();
  proxy_thread.join();
  EXPECT_GT(proxy.dropped(), 0u) << "chaos proxy injected no loss — test proves nothing";

  for (const auto& report : reports) {
    ASSERT_TRUE(report.welcomed) << report.error;
    EXPECT_TRUE(report.udp_bound) << "UdpHello retry did not survive loss: " << report.error;
    EXPECT_FALSE(report.duplicate_event) << "reliable dedup broke under duplication";
    EXPECT_EQ(count_kind(report, protocol::EventKind::kLaunch), 4u);
    EXPECT_EQ(count_kind(report, protocol::EventKind::kRocketResolved), 4u);
    // 5s at 30Hz = ~150 ticks; with 10% loss anything past 90 proves flow.
    EXPECT_GE(report.snapshot_ticks, 90u);
  }
  EXPECT_EQ(event_multiset(reports[0]), event_multiset(reports[1]));
}

}  // namespace
}  // namespace seashield
