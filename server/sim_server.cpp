#include "server/sim_server.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <random>
#include <variant>

#include "core/logger.h"
#include "core/math.h"
#include "sim/constants.h"
#include "sim/world.h"

namespace seashield::server {
namespace {

using namespace std::chrono;

// Entities per snapshot datagram: header (12B) + envelope (3B) + snapshot
// fields (10B) leave room for 58 × 20B records under the 1200B budget.
constexpr std::size_t kSnapshotFixedBytes = 12 + 3 + 10;
constexpr std::size_t kEntitiesPerBatch =
    (protocol::kMaxDatagramBytes - kSnapshotFixedBytes) / protocol::kEntityRecordBytes;

// Operator-input sanity ranges. A request outside these is an application
// error (broken UI, fuzzing), rejected without killing the console: unlike a
// framing violation, the transport itself is still healthy.
bool valid_fire_request(const protocol::FireRequest& fire) {
  return std::isfinite(fire.azimuth_rad) && std::isfinite(fire.elevation_rad) &&
         fire.elevation_rad >= 0.0 && fire.elevation_rad <= math::deg_to_rad(90.0) &&
         fire.salvo_count >= 1 && fire.salvo_count <= 64 && std::isfinite(fire.dispersion_mrad) &&
         fire.dispersion_mrad >= 0.0 && fire.dispersion_mrad <= 50.0 &&
         std::isfinite(fire.launch_interval_s) && fire.launch_interval_s >= 0.0 &&
         fire.launch_interval_s <= 1.0;
}

std::uint64_t random_seed() {
  std::random_device rd;
  return (static_cast<std::uint64_t>(rd()) << 32) | rd();
}

}  // namespace

SimServer::SimServer(SimServerConfig config)
    : config_(std::move(config)), token_rng_(random_seed()) {}

SimServer::~SimServer() { stop(); }

std::uint64_t SimServer::addr_key(const sockaddr_in& addr) {
  return (static_cast<std::uint64_t>(addr.sin_addr.s_addr) << 16) | addr.sin_port;
}

double SimServer::now_s() const {
  return duration<double>(steady_clock::now() - start_time_).count();
}

bool SimServer::start() {
  if (started_) {
    return false;
  }
  start_time_ = steady_clock::now();
  loop_ = net::EventLoop::create();
  if (!loop_) {
    return false;
  }
  // Registration happens on this thread BEFORE the loop thread spawns —
  // same pattern the P1 integration tests rely on (design doc §7).
  acceptor_ = std::make_unique<net::Acceptor>(
      *loop_, [this](UniqueFd fd, const sockaddr_in& peer) { on_connection(std::move(fd), peer); });
  if (!acceptor_->listen(config_.tcp_port)) {
    return false;
  }
  tcp_port_ = acceptor_->port();
  udp_ = std::make_unique<net::UdpEndpoint>(*loop_);
  if (!udp_->open(config_.udp_port,
                  [this](std::span<const std::uint8_t> payload, const sockaddr_in& from) {
                    on_udp_datagram(payload, from);
                  })) {
    return false;
  }
  udp_port_ = udp_->port();

  started_ = true;
  io_running_.store(true);
  sim_running_.store(true);
  io_thread_ = std::thread([this] { io_thread_main(); });
  sim_thread_ = std::thread([this] { sim_thread_main(); });
  SS_LOG_INFO("sim server up: tcp=%u udp=%u duration=%.0fs", tcp_port_, udp_port_,
              config_.scenario.duration_s);
  return true;
}

void SimServer::stop() {
  if (!started_) {
    return;
  }
  started_ = false;
  sim_running_.store(false);
  if (sim_thread_.joinable()) {
    sim_thread_.join();
  }
  io_running_.store(false);
  loop_->wakeup();
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
  const std::uint64_t ticks = stats_.ticks.load();
  if (ticks > 0) {
    SS_LOG_INFO("tick cost: avg=%.0fus max=%lluus over-8ms=%llu/%llu",
                static_cast<double>(stats_.tick_busy_sum_us.load()) / static_cast<double>(ticks),
                static_cast<unsigned long long>(stats_.tick_busy_max_us.load()),
                static_cast<unsigned long long>(stats_.tick_busy_over_8ms.load()),
                static_cast<unsigned long long>(ticks));
  }
  if (!config_.journal_path.empty()) {
    std::ofstream out(config_.journal_path);
    out << journal_.serialize();
    SS_LOG_INFO("journal written to %s (%zu entries)", config_.journal_path.c_str(),
                journal_.entries().size());
  }
}

std::string SimServer::journal_text() const { return journal_.serialize(); }

// --- I/O thread ----------------------------------------------------------------

void SimServer::io_thread_main() {
  while (io_running_.load()) {
    loop_->run_once(10);
    pump();
  }
  // Final reap so sessions close cleanly before the loop is destroyed.
  reap_transports();
}

void SimServer::pump() {
  const double now = now_s();

  // 1. Drain the sim thread's output and fan it out.
  SimOutput output;
  while (sim_to_net_.pop(output)) {
    dispatch_sim_output(output);
  }

  // 2. Handshake timeout: a transport that never says hello does not get to
  //    hold a descriptor forever (design doc §5.1 idle-connection note).
  for (const auto& [transport_id, accepted_at] : pending_hello_) {
    if (now - accepted_at > config_.handshake_timeout_s) {
      if (auto it = transports_.find(transport_id); it != transports_.end()) {
        it->second->close("handshake timeout");
      }
    }
  }

  // 3. Per-session reliable-channel upkeep: retransmissions, delayed acks,
  //    and the liveness verdict.
  for (auto& [token, session] : sessions_) {
    if (!session.udp_bound) {
      continue;
    }
    if (session.endpoint->peer_timed_out(now)) {
      stats_.udp_unbound_timeouts.fetch_add(1);
      unbind_udp(session, "reliable-channel timeout");
      continue;
    }
    flush_session(session, now);
  }

  reap_transports();
}

void SimServer::flush_session(LogicalSession& session, double now) {
  const sockaddr_in addr = session.udp_addr;
  session.endpoint->flush(
      now, [&](std::span<const std::uint8_t> datagram) { udp_->send_to(datagram, addr); });
}

void SimServer::dispatch_sim_output(const SimOutput& output) {
  // Encode each payload ONCE, then hand the same bytes to every session's
  // endpoint (which wraps them in its own packet header).
  std::vector<std::vector<std::uint8_t>> snapshot_batches;
  if (output.has_snapshot) {
    const std::size_t total = output.entities.size();
    for (std::size_t first = 0; first == 0 || first < total; first += kEntitiesPerBatch) {
      protocol::Snapshot snap;
      snap.tick = output.tick;
      snap.phase = output.phase;
      snap.total_entities = static_cast<std::uint16_t>(total);
      snap.first_index = static_cast<std::uint16_t>(first);
      const std::size_t count = std::min(kEntitiesPerBatch, total - first);
      snap.entities.assign(output.entities.begin() + static_cast<std::ptrdiff_t>(first),
                           output.entities.begin() + static_cast<std::ptrdiff_t>(first + count));
      snapshot_batches.push_back(protocol::encode_payload(snap));
      if (total == 0) {
        break;  // Degenerate empty snapshot still announces the tick.
      }
    }
  }
  std::vector<std::vector<std::uint8_t>> event_payloads;
  event_payloads.reserve(output.events.size());
  for (const auto& event : output.events) {
    event_payloads.push_back(protocol::encode_payload(event));
  }

  std::vector<std::uint64_t> overflowed;
  for (auto& [token, session] : sessions_) {
    if (!session.udp_bound) {
      continue;
    }
    for (const auto& batch : snapshot_batches) {
      session.endpoint->send_unreliable(protocol::MsgType::kSnapshot, batch);
      stats_.snapshot_batches_sent.fetch_add(1);
    }
    for (const auto& payload : event_payloads) {
      if (!session.endpoint->send_reliable(protocol::MsgType::kEngagementEvent, payload)) {
        // Same philosophy as the TCP send-queue cap (§6.3): a peer that lets
        // 256 events pile up unacknowledged is unrecoverable.
        overflowed.push_back(token);
        break;
      }
      stats_.events_sent.fetch_add(1);
    }
  }
  for (const std::uint64_t token : overflowed) {
    auto it = sessions_.find(token);
    if (it != sessions_.end()) {
      unbind_udp(it->second, "reliable-channel overflow");
    }
  }
}

void SimServer::unbind_udp(LogicalSession& session, const char* reason) {
  SS_LOG_WARN("session %llx: udp unbound (%s)", static_cast<unsigned long long>(session.token),
              reason);
  if (session.udp_bound) {
    udp_index_.erase(addr_key(session.udp_addr));
  }
  session.udp_bound = false;
  // Fresh endpoint for the next incarnation: sequence spaces are born in
  // pairs, so the stale one must not survive a rebind. Undelivered events of
  // the old incarnation die with it — the full snapshot resync covers state,
  // and this limit is documented in the protocol spec.
  session.endpoint = make_endpoint();
}

void SimServer::reap_transports() {
  for (const std::uint64_t id : dead_transports_) {
    transports_.erase(id);
    pending_hello_.erase(id);
    if (auto it = attachments_.find(id); it != attachments_.end()) {
      if (auto session = sessions_.find(it->second); session != sessions_.end()) {
        session->second.transport_id = 0;  // 권한 잠금; the role waits for the token.
      }
      attachments_.erase(it);
    }
  }
  if (!dead_transports_.empty() && acceptor_->paused()) {
    acceptor_->resume();
  }
  dead_transports_.clear();
}

void SimServer::on_connection(UniqueFd fd, const sockaddr_in&) {
  if (transports_.size() >= config_.max_clients) {
    return;  // Admission control; RAII closes the socket.
  }
  const std::uint64_t id = next_transport_id_++;
  auto transport = std::make_unique<net::TcpSession>(*loop_, std::move(fd), id, config_.send_cap);
  net::TcpSession* raw = transport.get();
  transports_[id] = std::move(transport);
  pending_hello_[id] = now_s();
  raw->start(
      [this](net::TcpSession& t, std::span<const std::uint8_t> frame) { on_frame(t, frame); },
      [this](net::TcpSession& t, const char* reason) { on_transport_closed(t, reason); });
}

void SimServer::on_transport_closed(net::TcpSession& transport, const char* reason) {
  SS_LOG_INFO("transport %llu closed: %s", static_cast<unsigned long long>(transport.id()),
              reason);
  dead_transports_.push_back(transport.id());
}

void SimServer::on_frame(net::TcpSession& transport, std::span<const std::uint8_t> frame) {
  const auto message = protocol::decode_control_frame(frame);
  if (!message) {
    transport.close("protocol violation: bad control frame");
    return;
  }
  if (const auto* hello = std::get_if<protocol::ClientHello>(&*message)) {
    handle_hello(transport, *hello);
  } else if (const auto* fire = std::get_if<protocol::FireRequest>(&*message)) {
    handle_fire(transport, *fire);
  } else {
    // Welcome/Reject are server-to-client only.
    transport.close("protocol violation: unexpected control message");
  }
}

void SimServer::reject_and_close(net::TcpSession& transport, protocol::RejectReason reason) {
  // Best effort: a small frame normally flushes in one write before close().
  transport.send(protocol::encode_control_frame(protocol::ServerReject{reason}));
  transport.close("handshake rejected");
}

bool SimServer::role_available(protocol::Role role) const {
  if (role == protocol::Role::kObserver) {
    return true;
  }
  for (const auto& [token, session] : sessions_) {
    const protocol::Role taken = session.role;
    if (taken == protocol::Role::kObserver) {
      continue;
    }
    // kSolo holds every seat, so it conflicts with any non-observer (and
    // vice versa); commander/weapons conflict with their own kind.
    if (role == protocol::Role::kSolo || taken == protocol::Role::kSolo || taken == role) {
      return false;
    }
  }
  return true;
}

std::unique_ptr<protocol::ReliableEndpoint> SimServer::make_endpoint() const {
  protocol::EndpointConfig endpoint_config;
  endpoint_config.peer_timeout_s = config_.reliable_peer_timeout_s;
  return std::make_unique<protocol::ReliableEndpoint>(endpoint_config);
}

std::uint64_t SimServer::make_token() {
  for (;;) {
    const std::uint64_t token =
        (static_cast<std::uint64_t>(token_rng_.next()) << 32) | token_rng_.next();
    if (token != 0 && sessions_.find(token) == sessions_.end()) {
      return token;
    }
  }
}

void SimServer::handle_hello(net::TcpSession& transport, const protocol::ClientHello& hello) {
  if (attachments_.contains(transport.id())) {
    transport.close("protocol violation: duplicate hello");
    return;
  }
  if (hello.protocol_version != protocol::kProtocolVersion) {
    reject_and_close(transport, protocol::RejectReason::kVersionMismatch);
    return;
  }

  LogicalSession* session = nullptr;
  if (hello.token != 0) {
    // Reconnect path (charter §4.8): the token restores the reserved role.
    auto it = sessions_.find(hello.token);
    if (it == sessions_.end()) {
      reject_and_close(transport, protocol::RejectReason::kBadToken);
      return;
    }
    if (it->second.transport_id != 0) {
      reject_and_close(transport, protocol::RejectReason::kRoleTaken);  // Seat is live.
      return;
    }
    session = &it->second;
    unbind_udp(*session, "reconnect");  // New incarnation; client re-binds UDP.
    stats_.sessions_reattached.fetch_add(1);
  } else {
    if (!role_available(hello.role)) {
      reject_and_close(transport, protocol::RejectReason::kRoleTaken);
      return;
    }
    const std::uint64_t token = make_token();
    LogicalSession fresh;
    fresh.token = token;
    fresh.role = hello.role;
    fresh.endpoint = make_endpoint();
    session = &sessions_.emplace(token, std::move(fresh)).first->second;
    stats_.sessions_created.fetch_add(1);
  }

  session->transport_id = transport.id();
  attachments_[transport.id()] = session->token;
  pending_hello_.erase(transport.id());

  protocol::ServerWelcome welcome;
  welcome.token = session->token;
  welcome.role = session->role;
  welcome.udp_port = udp_port_;
  welcome.tick_rate_hz = static_cast<std::uint16_t>(sim::kTickRateHz);
  welcome.snapshot_rate_hz = static_cast<std::uint16_t>(sim::kTickRateHz / 2);
  welcome.weather_summary = config_.scenario.config.weather.describe();
  transport.send(protocol::encode_control_frame(welcome));
  SS_LOG_INFO("session %llx: role %u attached to transport %llu",
              static_cast<unsigned long long>(session->token),
              static_cast<unsigned>(session->role),
              static_cast<unsigned long long>(transport.id()));
}

void SimServer::handle_fire(net::TcpSession& transport, const protocol::FireRequest& fire) {
  const auto attachment = attachments_.find(transport.id());
  if (attachment == attachments_.end()) {
    transport.close("protocol violation: fire before hello");
    return;
  }
  const LogicalSession& session = sessions_.at(attachment->second);
  const bool may_fire =
      session.role == protocol::Role::kWeapons || session.role == protocol::Role::kSolo;
  if (!may_fire || !valid_fire_request(fire)) {
    stats_.commands_rejected.fetch_add(1);
    SS_LOG_WARN("session %llx: fire request rejected (%s)",
                static_cast<unsigned long long>(session.token),
                may_fire ? "out of range" : "role lacks weapons authority");
    return;
  }
  SimCommand command;
  command.fire.azimuth_rad = fire.azimuth_rad;
  command.fire.elevation_rad = fire.elevation_rad;
  command.fire.salvo_count = fire.salvo_count;
  command.fire.dispersion_mrad = fire.dispersion_mrad;
  command.fire.launch_interval_s = fire.launch_interval_s;
  if (!net_to_sim_.push(std::move(command))) {
    stats_.commands_rejected.fetch_add(1);  // Ring full: operator can resend.
    return;
  }
  stats_.commands_accepted.fetch_add(1);
}

void SimServer::on_udp_datagram(std::span<const std::uint8_t> payload, const sockaddr_in& from) {
  auto indexed = udp_index_.find(addr_key(from));
  if (indexed == udp_index_.end()) {
    try_udp_bind(payload, from);
    indexed = udp_index_.find(addr_key(from));
    if (indexed == udp_index_.end()) {
      return;  // Not a valid bind attempt; drop silently (LAN trust boundary).
    }
  }
  LogicalSession& session = sessions_.at(indexed->second);
  session.last_udp_seen_s = now_s();
  session.endpoint->on_datagram(
      now_s(), payload, [&](protocol::MsgType type, std::span<const std::uint8_t> body) {
        route_data_message(session, type, body);
      });
}

void SimServer::try_udp_bind(std::span<const std::uint8_t> payload, const sockaddr_in& from) {
  // Stateless peek: header + unreliable envelopes need no endpoint state.
  // Deliberate double-parse — after binding, the SAME datagram goes through
  // the session's endpoint (on_udp_datagram), which is what records its
  // sequence and re-acks the hello. This peek only answers "whose is it?".
  protocol::Reader r(payload);
  const std::uint16_t magic = r.u16();
  const std::uint8_t version = r.u8();
  const std::uint8_t channel = r.u8() & ~protocol::kAckValidFlag;
  r.u16();  // seq
  r.u16();  // ack
  r.u32();  // ack_bits
  if (!r.ok() || magic != protocol::kPacketMagic || version != protocol::kProtocolVersion ||
      channel != static_cast<std::uint8_t>(protocol::Channel::kUnreliable)) {
    return;
  }
  while (r.ok() && r.remaining() > 0) {
    const auto type = static_cast<protocol::MsgType>(r.u8());
    const std::uint16_t length = r.u16();
    const auto body = r.bytes(length);
    if (!r.ok()) {
      return;
    }
    if (type != protocol::MsgType::kUdpHello) {
      continue;
    }
    protocol::Reader body_reader(body);
    const auto hello = protocol::UdpHello::decode(body_reader);
    if (!hello || !body_reader.finished()) {
      return;
    }
    const auto session_it = sessions_.find(hello->token);
    if (session_it == sessions_.end()) {
      return;  // Unknown token: spoof surface acknowledged in the spec.
    }
    LogicalSession& session = session_it->second;
    if (session.udp_bound) {
      udp_index_.erase(addr_key(session.udp_addr));  // Client moved ports.
    }
    session.udp_addr = from;
    session.udp_bound = true;
    udp_index_[addr_key(from)] = session.token;
    SS_LOG_INFO("session %llx: udp bound", static_cast<unsigned long long>(session.token));
    return;
  }
}

void SimServer::route_data_message(LogicalSession& session, protocol::MsgType type,
                                   std::span<const std::uint8_t>) {
  switch (type) {
    case protocol::MsgType::kUdpHello:
      // Idempotent re-ack: the client repeats the hello until this lands.
      session.endpoint->send_unreliable(protocol::MsgType::kUdpHelloAck, {});
      break;
    case protocol::MsgType::kKeepalive:
      break;  // Its packet header (acks) already did the work.
    default:
      break;  // Client-to-server snapshots/events do not exist; ignore.
  }
}

// --- simulation thread -----------------------------------------------------------

void SimServer::sim_thread_main() {
  sim::World world(config_.scenario.config);
  const auto duration_ticks =
      static_cast<std::uint64_t>(std::llround(config_.scenario.duration_s * sim::kTickRateHz));
  const auto tick_period = nanoseconds(16'666'667);  // 1/60 s.

  std::size_t seen_results = 0;
  std::size_t seen_rockets = 0;
  bool target_destroyed_sent = false;
  bool end_sent = false;
  std::uint64_t cadence = 0;
  auto next_tick_at = steady_clock::now();

  while (sim_running_.load()) {
    // 1. Inputs: drain, journal, queue — order fixed (charter §4.6).
    SimCommand command;
    while (net_to_sim_.pop(command)) {
      journal_.record(world.tick(), command.fire);
      world.queue_fire(command.fire);
    }

    const auto work_started = steady_clock::now();

    // 2. Step while the engagement runs; afterwards the world freezes but
    //    snapshots keep flowing so late/rejoining consoles converge.
    // Anything that happens DURING step() (launches, target kill) happens at
    // the pre-increment tick — the same clock RocketResult::end_tick uses.
    const auto step_tick = static_cast<std::uint32_t>(world.tick());
    const bool running = world.tick() < duration_ticks;
    if (running) {
      world.step();
      stats_.ticks.fetch_add(1);
    }

    // 3. Events since the last tick.
    SimOutput output;
    const auto& rockets = world.rockets();
    for (; seen_rockets < rockets.size(); ++seen_rockets) {
      protocol::EngagementEvent event;
      event.tick = step_tick;
      event.kind = protocol::EventKind::kLaunch;
      event.rocket_id = static_cast<std::uint16_t>(rockets[seen_rockets].id);
      output.events.push_back(event);
    }
    const auto& results = world.results();
    for (; seen_results < results.size(); ++seen_results) {
      const sim::RocketResult& result = results[seen_results];
      protocol::EngagementEvent event;
      event.tick = static_cast<std::uint32_t>(result.end_tick);
      event.kind = protocol::EventKind::kRocketResolved;
      event.rocket_id = static_cast<std::uint16_t>(result.rocket_id);
      event.miss_distance_m = static_cast<float>(result.miss_distance_m);
      event.detonated = result.detonated;
      event.killed = result.killed;
      output.events.push_back(event);
    }
    if (!target_destroyed_sent && world.target().destroyed()) {
      target_destroyed_sent = true;
      protocol::EngagementEvent event;
      event.tick = step_tick;
      event.kind = protocol::EventKind::kTargetDestroyed;
      output.events.push_back(event);
    }
    if (!end_sent && !running) {
      end_sent = true;
      protocol::EngagementEvent event;
      event.tick = static_cast<std::uint32_t>(world.tick());
      event.kind = protocol::EventKind::kEngagementEnd;
      output.events.push_back(event);
    }

    // 4. Snapshot at half the tick rate (60Hz sim -> 30Hz send, charter §6).
    if (cadence % 2 == 0) {
      output.has_snapshot = true;
      output.tick = static_cast<std::uint32_t>(world.tick());
      output.phase =
          running ? protocol::EngagementPhase::kRunning : protocol::EngagementPhase::kEnded;
      protocol::EntityRecord target_record;
      target_record.id = 0;
      target_record.kind = protocol::EntityKind::kTarget;
      target_record.state = world.target().destroyed() ? 1 : 0;
      const math::Vec3& target_pos = world.target().position();
      const math::Vec3 target_vel = world.target().velocity();
      target_record.pos_x = target_pos.x;
      target_record.pos_y = target_pos.y;
      target_record.pos_z = target_pos.z;
      target_record.vel_x = target_vel.x;
      target_record.vel_y = target_vel.y;
      target_record.vel_z = target_vel.z;
      output.entities.push_back(target_record);
      for (const sim::Rocket& rocket : rockets) {
        if (!rocket.alive) {
          continue;  // Resolved rockets live on as events, not entities.
        }
        protocol::EntityRecord record;
        record.id = static_cast<std::uint16_t>(rocket.id);
        record.kind = protocol::EntityKind::kRocket;
        record.state = rocket.state.age_s < config_.scenario.config.rocket.burn_time_s ? 0 : 1;
        record.pos_x = rocket.state.position.x;
        record.pos_y = rocket.state.position.y;
        record.pos_z = rocket.state.position.z;
        record.vel_x = rocket.state.velocity.x;
        record.vel_y = rocket.state.velocity.y;
        record.vel_z = rocket.state.velocity.z;
        output.entities.push_back(record);
      }
    }
    ++cadence;

    // 5. Publish and wake the I/O thread ("push, then wakeup" — design §4.3).
    if (output.has_snapshot || !output.events.empty()) {
      if (sim_to_net_.push(std::move(output))) {
        loop_->wakeup();
      } else {
        stats_.sim_output_dropped.fetch_add(1);
      }
    }

    const auto busy_us = static_cast<std::uint64_t>(
        duration_cast<microseconds>(steady_clock::now() - work_started).count());
    stats_.tick_busy_sum_us.fetch_add(busy_us);
    if (busy_us > stats_.tick_busy_max_us.load()) {
      stats_.tick_busy_max_us.store(busy_us);  // Sim thread is the only writer.
    }
    if (busy_us > 8000) {
      stats_.tick_busy_over_8ms.fetch_add(1);  // §10.3: p99 budget is 8ms.
    }

    // 6. Fixed-rate pacing on absolute deadlines (no cumulative drift). If
    //    the host stalls long enough to bury us, resync instead of sprinting
    //    through a burst of catch-up ticks.
    next_tick_at += tick_period;
    const auto now = steady_clock::now();
    if (now < next_tick_at) {
      std::this_thread::sleep_until(next_tick_at);
    } else if (now - next_tick_at > 10 * tick_period) {
      SS_LOG_WARN("sim thread fell behind by %lld ms; resyncing",
                  static_cast<long long>(duration_cast<milliseconds>(now - next_tick_at).count()));
      next_tick_at = now;
    }
  }
}

}  // namespace seashield::server
