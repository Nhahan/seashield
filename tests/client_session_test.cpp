#include "client/core/client_session.h"

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <thread>

#include "client/core/interp_buffer.h"
#include "core/math.h"
#include "net/socket_util.h"
#include "server/sim_server.h"
#include "sim/scenario.h"

// P5 K1/K2 end-to-end: the PRODUCTION client engine (ClientSession — the
// exact code the UE network thread runs) against the real server, feeding
// the assembler + interpolation buffer pipeline the UE render path reads.
namespace seashield {
namespace {

using namespace std::chrono_literals;

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
radar_scan_period_s = 0.5
)";

TEST(ClientSessionTest, ProductionPipelineConsumesALiveEngagement) {
  net::ignore_sigpipe();
  std::string error;
  const auto scenario = sim::load_scenario_text(kScenarioText, &error);
  ASSERT_TRUE(scenario.has_value()) << error;
  server::SimServerConfig server_config;
  server_config.tcp_port = 0;
  server_config.udp_port = 0;
  server_config.scenario = *scenario;
  server::SimServer server(server_config);
  ASSERT_TRUE(server.start());

  std::mutex mutex;
  bool welcomed = false;
  protocol::ServerWelcome welcome;
  std::uint64_t launches = 0;
  std::uint64_t valid_solutions = 0;
  client::SnapshotAssembler assembler;
  client::InterpolationBuffer interp;
  std::uint64_t completed_snapshots = 0;
  std::string session_error;

  client::ClientSessionConfig config;
  config.tcp_port = server.tcp_port();
  config.role = protocol::Role::kSolo;
  client::ClientSessionCallbacks callbacks;
  callbacks.on_welcome = [&](const protocol::ServerWelcome& w) {
    const std::lock_guard<std::mutex> lock(mutex);
    welcomed = true;
    welcome = w;
  };
  callbacks.on_snapshot = [&](const protocol::Snapshot& batch) {
    const std::lock_guard<std::mutex> lock(mutex);
    if (auto done = assembler.push(batch)) {
      ++completed_snapshots;
      interp.push(std::move(*done));
    }
  };
  callbacks.on_event = [&](const protocol::EngagementEvent& event) {
    const std::lock_guard<std::mutex> lock(mutex);
    if (event.kind == protocol::EventKind::kLaunch) {
      ++launches;
    }
  };
  callbacks.on_fire_solution = [&](const protocol::FireSolution& solution) {
    const std::lock_guard<std::mutex> lock(mutex);
    if (solution.valid) {
      ++valid_solutions;
    }
  };
  callbacks.on_error = [&](const std::string& what) {
    const std::lock_guard<std::mutex> lock(mutex);
    session_error = what;
  };

  client::ClientSession session(config, callbacks);
  std::thread net_thread([&] { session.run(); });

  // Wait for the kill chain to appear on the wire: welcome, snapshots, and
  // at least one converged fire solution for the confirmed track.
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  bool ready = false;
  while (!ready && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(50ms);
    const std::lock_guard<std::mutex> lock(mutex);
    ready = welcomed && completed_snapshots >= 5 && valid_solutions >= 1;
  }
  {
    const std::lock_guard<std::mutex> lock(mutex);
    ASSERT_TRUE(session_error.empty()) << session_error;
    ASSERT_TRUE(ready) << "welcomed=" << welcomed << " snapshots=" << completed_snapshots
                       << " valid_solutions=" << valid_solutions;
    EXPECT_GE(welcome.rain_intensity, 0.0);
    EXPECT_LE(welcome.rain_intensity, 1.0);
    EXPECT_GE(welcome.gust_sigma_mps, 0.0);
  }

  // The operator fires a manual low shot; the launch events come back.
  protocol::FireRequest fire;
  fire.azimuth_rad = math::deg_to_rad(45.0);
  fire.elevation_rad = math::deg_to_rad(1.0);
  fire.salvo_count = 4;
  fire.dispersion_mrad = 3.0;
  fire.launch_interval_s = 0.05;
  session.request_fire(fire);

  const auto fire_deadline = std::chrono::steady_clock::now() + 5s;
  std::uint64_t seen_launches = 0;
  while (seen_launches < 4 && std::chrono::steady_clock::now() < fire_deadline) {
    std::this_thread::sleep_for(50ms);
    const std::lock_guard<std::mutex> lock(mutex);
    seen_launches = launches;
  }
  EXPECT_EQ(seen_launches, 4u);

  // Render-path sample: the delayed clock yields entities (the target at
  // minimum), proving the assembler -> buffer -> sample chain end to end.
  {
    const std::lock_guard<std::mutex> lock(mutex);
    const auto render_tick = interp.render_tick(6.0);
    ASSERT_TRUE(render_tick.has_value());
    const auto sampled = interp.sample(*render_tick);
    EXPECT_FALSE(sampled.empty());
  }

  session.stop();
  net_thread.join();
  server.stop();
}

}  // namespace
}  // namespace seashield
