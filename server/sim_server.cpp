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

// Track-designation trim limits (see handle_fire).
bool valid_fire_offsets(const protocol::FireRequest& fire) {
  const double limit = math::deg_to_rad(15.0);
  return std::isfinite(fire.azimuth_rad) && std::isfinite(fire.elevation_rad) &&
         fire.azimuth_rad >= -limit && fire.azimuth_rad <= limit &&
         fire.elevation_rad >= -limit && fire.elevation_rad <= limit &&
         fire.salvo_count >= 1 && fire.salvo_count <= 64 && std::isfinite(fire.dispersion_mrad) &&
         fire.dispersion_mrad >= 0.0 && fire.dispersion_mrad <= 50.0 &&
         std::isfinite(fire.launch_interval_s) && fire.launch_interval_s >= 0.0 &&
         fire.launch_interval_s <= 1.0;
}

// Steering set-points: finite and within the held-control range. Out-of-range
// is an application error (broken UI), rejected without killing the console.
bool valid_ship_command(const protocol::ShipCommand& steer) {
  return std::isfinite(steer.rudder) && std::isfinite(steer.throttle) && steer.rudder >= -1.0 &&
         steer.rudder <= 1.0 && steer.throttle >= 0.0 && steer.throttle <= 1.0;
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
  if (!config_.replay_journal_text.empty()) {
    replay_journal_ = sim::Journal::parse(config_.replay_journal_text);
    if (!replay_journal_.has_value()) {
      SS_LOG_ERROR("replay journal failed to parse");
      return false;
    }
  }
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
    // Conservative p99 from the log-bucket histogram: the upper edge of the
    // first bucket whose cumulative count covers 99% of ticks.
    std::uint64_t cumulative = 0;
    for (std::size_t bucket = 0; bucket < SimServerStats::kTickHistBuckets; ++bucket) {
      cumulative += stats_.tick_busy_hist[bucket].load();
      if (cumulative * 100 >= ticks * 99) {
        SS_LOG_INFO("tick cost p99 <= %lluus (hist bucket %zu)",
                    static_cast<unsigned long long>(1ull << bucket), bucket);
        break;
      }
    }
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

std::vector<std::vector<std::uint8_t>> SimServer::encode_delta_batches(
    const SimOutput& output, const std::vector<protocol::EntityRecord>& base,
    std::uint32_t base_tick) const {
  std::map<std::uint32_t, const protocol::EntityRecord*> base_index;
  for (const protocol::EntityRecord& entity : base) {
    base_index[(static_cast<std::uint32_t>(entity.kind) << 16) | entity.id] = &entity;
  }
  const std::uint32_t dticks = output.tick - base_tick;
  std::vector<protocol::DeltaEntity> records;
  records.reserve(output.entities.size());
  for (const protocol::EntityRecord& current : output.entities) {
    const auto it =
        base_index.find((static_cast<std::uint32_t>(current.kind) << 16) | current.id);
    if (it == base_index.end()) {
      protocol::DeltaEntity escape;  // New since the baseline: full record.
      escape.id = current.id;
      escape.mask = static_cast<std::uint8_t>(
          (static_cast<std::uint8_t>(current.kind) << protocol::DeltaEntity::kKindShift) |
          protocol::DeltaEntity::kFullRecord);
      escape.full = current;
      records.push_back(escape);
    } else {
      records.push_back(
          protocol::make_delta_entity(*it->second, current, dticks, sim::kTickRateHz));
    }
  }

  // Greedy batching under the datagram budget (records vary 9..23 bytes).
  constexpr std::size_t kDeltaHeaderBytes = 4 + 4 + 1 + 2 + 2 + 1;
  const std::size_t budget =
      protocol::kMaxDatagramBytes - protocol::kPacketHeaderBytes - kDeltaHeaderBytes;
  std::vector<std::vector<std::uint8_t>> payloads;
  std::size_t first = 0;
  while (first < records.size()) {
    protocol::SnapshotDelta delta;
    delta.tick = output.tick;
    delta.base_tick = base_tick;
    delta.phase = output.phase;
    delta.total_entities = static_cast<std::uint16_t>(records.size());
    delta.first_index = static_cast<std::uint16_t>(first);
    std::size_t used = 0;
    while (first < records.size() && delta.entities.size() < 255) {
      const std::size_t size = records[first].encoded_size();
      if (used + size > budget) {
        break;
      }
      used += size;
      delta.entities.push_back(records[first]);
      ++first;
    }
    payloads.push_back(protocol::encode_payload(delta));
  }
  return payloads;
}

void SimServer::dispatch_sim_output(const SimOutput& output) {
  // Encode each payload ONCE, then hand the same bytes to every session's
  // endpoint (which wraps them in its own packet header).
  std::vector<std::vector<std::uint8_t>> snapshot_batches;
  if (output.has_snapshot) {
    const std::size_t total = output.entities.size();
    // The `first == 0` clause makes the loop emit at least one batch even
    // when total == 0, so clients still see the tick advance.
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
  // v4 delta baselines: remember the frame so later frames can be encoded
  // as residuals against whatever tick each client has acked.
  if (output.has_snapshot) {
    constexpr std::size_t kSnapshotRingFrames = 64;  // ~2 s at 30 Hz.
    snapshot_ring_.emplace_back(output.tick, output.entities);
    while (snapshot_ring_.size() > kSnapshotRingFrames) {
      snapshot_ring_.pop_front();
    }
  }

  std::vector<std::vector<std::uint8_t>> event_payloads;
  event_payloads.reserve(output.events.size());
  for (const auto& event : output.events) {
    event_payloads.push_back(protocol::encode_payload(event));
  }
  // v4: every event also lands in the permanent log that backs the bind-time
  // TCP catch-up (charter §5.8).
  event_log_.insert(event_log_.end(), output.events.begin(), output.events.end());
  std::vector<std::vector<std::uint8_t>> fire_solution_payloads;
  fire_solution_payloads.reserve(output.fire_solutions.size());
  for (const auto& solution : output.fire_solutions) {
    fire_solution_payloads.push_back(protocol::encode_payload(solution));
  }

  std::map<std::uint32_t, std::vector<std::vector<std::uint8_t>>> delta_cache;
  std::vector<std::uint64_t> overflowed;
  for (auto& [token, session] : sessions_) {
    if (!session.udp_bound) {
      continue;
    }
    // v4: a client whose acked baseline is still in the ring gets residual
    // deltas; everyone else (silent, behind, or delta disabled) gets full
    // snapshots. Each distinct baseline is encoded once and reused.
    bool delta_sent = false;
    if (output.has_snapshot && config_.scenario.snapshot_delta && session.has_ack &&
        session.acked_tick < output.tick && !output.entities.empty()) {
      auto cached = delta_cache.find(session.acked_tick);
      if (cached == delta_cache.end()) {
        const std::vector<protocol::EntityRecord>* base = nullptr;
        for (const auto& [ring_tick, ring_entities] : snapshot_ring_) {
          if (ring_tick == session.acked_tick) {
            base = &ring_entities;
            break;
          }
        }
        if (base != nullptr) {
          cached = delta_cache
                       .emplace(session.acked_tick,
                                encode_delta_batches(output, *base, session.acked_tick))
                       .first;
        }
      }
      if (cached != delta_cache.end()) {
        for (const auto& payload : cached->second) {
          session.endpoint->send_unreliable(protocol::MsgType::kSnapshotDelta, payload);
          stats_.delta_batches_sent.fetch_add(1);
        }
        delta_sent = true;
      } else {
        stats_.snapshot_full_fallbacks.fetch_add(1);  // Baseline left the ring.
      }
    }
    if (!delta_sent) {
      for (const auto& batch : snapshot_batches) {
        session.endpoint->send_unreliable(protocol::MsgType::kSnapshot, batch);
        stats_.snapshot_batches_sent.fetch_add(1);
      }
    }
    for (const auto& payload : fire_solution_payloads) {
      session.endpoint->send_unreliable(protocol::MsgType::kFireSolution, payload);
      stats_.fire_solutions_sent.fetch_add(1);
    }
    bool events_ok = true;
    for (const auto& payload : event_payloads) {
      if (!session.endpoint->send_reliable(protocol::MsgType::kEngagementEvent, payload)) {
        // Same philosophy as the TCP send-queue cap (§6.3): a peer that lets
        // 256 events pile up unacknowledged is unrecoverable.
        overflowed.push_back(token);
        events_ok = false;
        break;
      }
      stats_.events_sent.fetch_add(1);
    }
    if (events_ok) {
      session.events_conveyed = event_log_.size();  // Backlog cursor (v4).
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
  // pairs, so the stale one must not survive a rebind. Events possibly lost
  // with the old incarnation's in-flight window are covered by rewinding the
  // backlog cursor — the next bind replays from here over TCP (v4; the
  // client's dedup absorbs the overlap).
  session.endpoint = make_endpoint();
  session.events_conveyed = session.events_at_bind;
}

void SimServer::send_event_backlog(LogicalSession& session) {
  session.events_at_bind = session.events_conveyed;
  if (session.events_conveyed >= event_log_.size()) {
    return;
  }
  const auto transport = transports_.find(session.transport_id);
  if (transport == transports_.end()) {
    return;  // No live TCP transport: the next incarnation will catch up.
  }
  std::size_t cursor = session.events_conveyed;
  while (cursor < event_log_.size()) {
    protocol::EventBacklog backlog;
    const std::size_t count = std::min<std::size_t>(255, event_log_.size() - cursor);
    backlog.events.assign(event_log_.begin() + static_cast<std::ptrdiff_t>(cursor),
                          event_log_.begin() + static_cast<std::ptrdiff_t>(cursor + count));
    transport->second->send(protocol::encode_control_frame(backlog));
    stats_.backlog_events_sent.fetch_add(count);
    cursor += count;
  }
  session.events_conveyed = event_log_.size();
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
  } else if (const auto* steer = std::get_if<protocol::ShipCommand>(&*message)) {
    handle_steer(transport, *steer);
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
    session->has_ack = false;  // The client restarts its assembler too (v4).
    session->acked_tick = 0;
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

  session->udp_nonce = ++udp_nonce_counter_;  // Fresh per incarnation (v4).
  protocol::ServerWelcome welcome;
  welcome.token = session->token;
  welcome.role = session->role;
  welcome.udp_nonce = session->udp_nonce;
  welcome.udp_port = udp_port_;
  welcome.tick_rate_hz = static_cast<std::uint16_t>(sim::kTickRateHz);
  welcome.snapshot_rate_hz = static_cast<std::uint16_t>(sim::kTickRateHz / 2);
  welcome.weather_summary = config_.scenario.config.weather.describe();
  const sim::Weather& weather = config_.scenario.config.weather;
  if (!weather.wind_layers.empty()) {
    welcome.surface_wind_east_mps = weather.wind_layers.front().velocity.x;
    welcome.surface_wind_north_mps = weather.wind_layers.front().velocity.y;
  }
  welcome.rain_intensity = weather.rain_intensity;
  welcome.gust_sigma_mps = weather.turbulence_intensity * weather.surface_wind_speed();
  welcome.humidity = weather.humidity;
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
  if (replay_journal_.has_value()) {
    stats_.commands_rejected.fetch_add(1);
    SS_LOG_WARN("session %llx: fire request refused — replay mode",
                static_cast<unsigned long long>(session.token));
    return;
  }
  const bool may_fire =
      session.role == protocol::Role::kWeapons || session.role == protocol::Role::kSolo;
  // Track-designated fire reinterprets az/el as operator trim on the
  // server-computed solution: bounded to ±15° (anything larger is not a
  // correction, it is a different shot — fire manually instead).
  const bool valid = fire.track_id != 0 ? valid_fire_offsets(fire) : valid_fire_request(fire);
  if (!may_fire || !valid) {
    stats_.commands_rejected.fetch_add(1);
    SS_LOG_WARN("session %llx: fire request rejected (%s)",
                static_cast<unsigned long long>(session.token),
                may_fire ? "out of range" : "role lacks weapons authority");
    return;
  }
  SimCommand command;
  command.kind = SimCommand::Kind::kFire;
  command.track_id = fire.track_id;
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

void SimServer::handle_steer(net::TcpSession& transport, const protocol::ShipCommand& steer) {
  const auto attachment = attachments_.find(transport.id());
  if (attachment == attachments_.end()) {
    transport.close("protocol violation: steer before hello");
    return;
  }
  const LogicalSession& session = sessions_.at(attachment->second);
  if (replay_journal_.has_value()) {
    stats_.commands_rejected.fetch_add(1);  // The journal drives the helm in replay.
    return;
  }
  const bool may_steer =
      session.role == protocol::Role::kWeapons || session.role == protocol::Role::kSolo;
  if (!may_steer || !valid_ship_command(steer)) {
    stats_.commands_rejected.fetch_add(1);
    return;
  }
  SimCommand command;
  command.kind = SimCommand::Kind::kSteer;
  command.steer.rudder = steer.rudder;
  command.steer.throttle = steer.throttle;
  if (!net_to_sim_.push(std::move(command))) {
    stats_.commands_rejected.fetch_add(1);  // Ring full: the next set-point resends.
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
    if (hello->nonce != session.udp_nonce) {
      // A pre-reconnect socket's late hello must not steal the binding of
      // the live incarnation (P3 backlog: transport-incarnation 결속).
      stats_.stale_udp_hellos.fetch_add(1);
      return;
    }
    if (session.udp_bound) {
      udp_index_.erase(addr_key(session.udp_addr));  // Client moved ports.
    }
    session.udp_addr = from;
    session.udp_bound = true;
    udp_index_[addr_key(from)] = session.token;
    SS_LOG_INFO("session %llx: udp bound", static_cast<unsigned long long>(session.token));
    send_event_backlog(session);
    return;
  }
}

void SimServer::route_data_message(LogicalSession& session, protocol::MsgType type,
                                   std::span<const std::uint8_t> payload) {
  switch (type) {
    case protocol::MsgType::kUdpHello:
      // Idempotent re-ack: the client repeats the hello until this lands.
      session.endpoint->send_unreliable(protocol::MsgType::kUdpHelloAck, {});
      break;
    case protocol::MsgType::kKeepalive:
      break;  // Its packet header (acks) already did the work.
    case protocol::MsgType::kSnapshotAck: {
      const auto message = protocol::decode_data_message(type, payload);
      const auto* ack = message ? std::get_if<protocol::SnapshotAck>(&*message) : nullptr;
      if (ack != nullptr && (!session.has_ack || ack->tick > session.acked_tick)) {
        session.acked_tick = ack->tick;  // Monotonic: a late ack cannot rewind.
        session.has_ack = true;
      }
      break;
    }
    default:
      break;  // Client-to-server snapshots/events do not exist; ignore.
  }
}

// --- simulation thread -----------------------------------------------------------

void SimServer::sim_thread_main() {
  if (config_.scenario.game_mode) {
    game_thread_main();  // Survival waves — a wholly separate lifecycle.
    return;
  }
  sim::World world(config_.scenario.config);
  const auto duration_ticks =
      static_cast<std::uint64_t>(std::llround(config_.scenario.duration_s * sim::kTickRateHz));
  const auto tick_period = nanoseconds(16'666'667);  // 1/60 s.

  std::size_t seen_results = 0;
  std::size_t seen_rockets = 0;
  std::size_t seen_track_events = 0;
  std::size_t replay_cursor = 0;
  bool target_destroyed_sent = false;
  bool end_sent = false;
  std::uint64_t cadence = 0;
  // Per-track backoff after a FAILED streaming solve: non-convergent
  // geometry burns the solver's full iteration budget (tens of ms measured,
  // P6 perf report), so the sim thread pays that at most once per cooldown
  // while converged tracks keep their normal cadence.
  constexpr std::uint64_t kSolveBackoffTicks = 5 * sim::kTickRateHz;
  std::map<std::uint32_t, std::uint64_t> solve_backoff_until;
  // Fire-solution cadence (scenario fire_solution_rate_hz, 0 = off): one PIP
  // solve integrates a full trajectory, so riding the 30Hz snapshot cadence
  // would threaten the 16.6ms tick budget (§10.3).
  const std::uint64_t fire_solution_interval =
      config_.scenario.fire_solution_rate_hz > 0.0
          ? std::max<std::uint64_t>(
                1, static_cast<std::uint64_t>(
                       sim::kTickRateHz / config_.scenario.fire_solution_rate_hz + 0.5))
          : 0;
  auto next_tick_at = steady_clock::now();

  while (sim_running_.load()) {
    // 1. Inputs: drain, journal, queue — order fixed (charter §4.6). In
    //    replay mode the parsed journal IS the input stream, applied at its
    //    recorded ticks (live fire was already refused on the I/O thread).
    if (replay_journal_.has_value()) {
      while (replay_cursor < replay_journal_->entries().size() &&
             replay_journal_->entries()[replay_cursor].tick == world.tick()) {
        const sim::JournalEntry& entry = replay_journal_->entries()[replay_cursor];
        if (entry.kind == sim::JournalEntry::Kind::kSteer) {
          world.queue_steer(entry.steer);
        } else {
          world.queue_fire(entry.command);
        }
        ++replay_cursor;
      }
    }
    SimCommand command;
    while (net_to_sim_.pop(command)) {
      if (command.kind == SimCommand::Kind::kSteer) {
        journal_.record_steer(world.tick(), command.steer);
        world.queue_steer(command.steer);
        continue;
      }
      sim::FireCommand fire = command.fire;
      if (command.track_id != 0) {
        // Resolve the designated track HERE: the tracker belongs to this
        // thread. az/el arrived as operator trim on the solution.
        // This is also the staleness gate for designated fire: solve_for_track
        // returns nullopt for a track that is gone, unconfirmed, or has coasted
        // past max_coast_scans (kStale), so an order against a stale/coasting
        // track is dropped (counted) rather than fired blind. The operator sees
        // it coming — the same gate makes the streamed solution invalid, so the
        // console reads "NO SOLUTION" before the trigger is ever pulled.
        const auto solution = world.solve_for_track(command.track_id);
        if (!solution.has_value()) {
          stats_.track_solution_failures.fetch_add(1);
          continue;
        }
        fire.azimuth_rad = solution->azimuth_rad + command.fire.azimuth_rad;
        fire.elevation_rad = solution->elevation_rad + command.fire.elevation_rad;
      }
      journal_.record(world.tick(), fire);
      world.queue_fire(fire);
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
      event.subject_id = static_cast<std::uint16_t>(rockets[seen_rockets].id);
      output.events.push_back(event);
    }
    const auto& results = world.results();
    for (; seen_results < results.size(); ++seen_results) {
      const sim::RocketResult& result = results[seen_results];
      protocol::EngagementEvent event;
      event.tick = static_cast<std::uint32_t>(result.end_tick);
      event.kind = protocol::EventKind::kRocketResolved;
      event.subject_id = static_cast<std::uint16_t>(result.rocket_id);
      event.miss_distance_m = static_cast<float>(result.miss_distance_m);
      event.detonated = result.detonated;
      event.killed = result.killed;
      output.events.push_back(event);
    }
    const auto& world_track_events = world.track_events();
    for (; seen_track_events < world_track_events.size(); ++seen_track_events) {
      const sim::TrackEvent& track_event = world_track_events[seen_track_events];
      // Initiations stay off the wire: the snapshot's tentative records
      // already show them (protocol/messages.h EventKind note).
      if (track_event.kind == sim::TrackEvent::Kind::kInitiated) {
        continue;
      }
      protocol::EngagementEvent event;
      event.tick = static_cast<std::uint32_t>(track_event.tick);
      event.kind = track_event.kind == sim::TrackEvent::Kind::kConfirmed
                       ? protocol::EventKind::kTrackConfirmed
                       : protocol::EventKind::kTrackLost;
      event.subject_id = static_cast<std::uint16_t>(track_event.track_id);
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
      append_world_snapshot(output, world);
    }

    // 4b. Fire solutions for confirmed tracks at their own low cadence. The
    // radius is quoted at the default dispersion; the console rescales it
    // linearly when the operator changes the setting (radius ∝ dispersion).
    if (running && fire_solution_interval != 0 && cadence % fire_solution_interval == 0) {
      for (const sim::Track& track : world.tracker().tracks()) {
        if (track.status != sim::TrackStatus::kConfirmed) {
          continue;
        }
        const auto backoff = solve_backoff_until.find(track.id);
        if (backoff != solve_backoff_until.end() && world.tick() < backoff->second) {
          continue;  // Recently failed: the geometry will not change in 0.5s.
        }
        protocol::FireSolution fire_solution;
        fire_solution.tick = static_cast<std::uint32_t>(world.tick());
        fire_solution.track_id = static_cast<std::uint16_t>(track.id);
        if (const auto solution = world.solve_for_track(track.id)) {
          solve_backoff_until.erase(track.id);
          // The solver aims at the CV extrapolation of the estimate — that
          // IS the PIP under the track's motion model.
          const math::Vec3 pip =
              track.position() + track.velocity() * solution->time_of_flight_s;
          fire_solution.valid = true;
          fire_solution.pip_x = pip.x;
          fire_solution.pip_y = pip.y;
          fire_solution.pip_z = pip.z;
          fire_solution.time_of_flight_s = static_cast<float>(solution->time_of_flight_s);
          // Preview pattern radius at the DEFAULT dispersion (1σ = mrad ×
          // slant range). The streamed solution is computed continuously,
          // before any operator order exists, so it cannot know the chosen
          // dispersion; the console/PPI redraw the circle from the operator's
          // own ordered dispersion (client-design / SeaFireControlPanel).
          fire_solution.dispersion_radius_m =
              static_cast<float>(sim::FireCommand{}.dispersion_mrad * 1e-3 * pip.norm());
        } else {
          solve_backoff_until[track.id] = world.tick() + kSolveBackoffTicks;
        }
        // valid=false still goes out once per failure — the console gets its
        // "no solution" signal, then the backoff silences the retries.
        output.fire_solutions.push_back(fire_solution);
      }
    }
    ++cadence;

    // 5. Publish and wake the I/O thread ("push, then wakeup" — design §4.3).
    if (output.has_snapshot || !output.events.empty() || !output.fire_solutions.empty()) {
      if (sim_to_net_.push(std::move(output))) {
        loop_->wakeup();
      } else {
        stats_.sim_output_dropped.fetch_add(1);
      }
    }

    const auto busy_us = static_cast<std::uint64_t>(
        duration_cast<microseconds>(steady_clock::now() - work_started).count());
    stats_.tick_busy_sum_us.fetch_add(busy_us);
    stats_
        .tick_busy_hist[std::min<std::size_t>(SimServerStats::kTickHistBuckets - 1,
                                              std::bit_width(busy_us))]
        .fetch_add(1);
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

void SimServer::append_world_snapshot(SimOutput& output, const sim::World& world) const {
  // Own ship (id 0): the player's platform pose. Velocity carries the course;
  // the client derives the hull's heading from it (the camera aim is
  // world-referenced and independent). A fixed platform reports the origin.
  {
    protocol::EntityRecord ship_record;
    ship_record.id = 0;
    ship_record.kind = protocol::EntityKind::kOwnShip;
    const math::Vec3 ship_pos = world.ship_position();
    const math::Vec3 ship_vel = world.ship_velocity();
    ship_record.pos_x = ship_pos.x;
    ship_record.pos_y = ship_pos.y;
    ship_record.pos_z = ship_pos.z;
    ship_record.vel_x = ship_vel.x;
    ship_record.vel_y = ship_vel.y;
    ship_record.vel_z = ship_vel.z;
    output.entities.push_back(ship_record);
  }
  // All concurrent targets, ids 0..N-1 (single-engagement = just id 0). The
  // client keys actors by (kind<<16|id), so multiple targets render directly.
  const auto& targets = world.targets();
  for (std::size_t i = 0; i < targets.size(); ++i) {
    const sim::Target& tgt = targets[i];
    protocol::EntityRecord target_record;
    target_record.id = static_cast<std::uint16_t>(i);
    target_record.kind = protocol::EntityKind::kTarget;
    target_record.state = tgt.destroyed() ? 1 : 0;
    const math::Vec3& target_pos = tgt.position();
    const math::Vec3 target_vel = tgt.velocity();
    target_record.pos_x = target_pos.x;
    target_record.pos_y = target_pos.y;
    target_record.pos_z = target_pos.z;
    target_record.vel_x = target_vel.x;
    target_record.vel_y = target_vel.y;
    target_record.vel_z = target_vel.z;
    output.entities.push_back(target_record);
  }
  for (const sim::Rocket& rocket : world.rockets()) {
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
  // The estimate stream (what consoles actually plot, charter §5.5): tracks
  // ride the same snapshot with their quality in the flags byte.
  for (const sim::Track& track : world.tracker().tracks()) {
    protocol::EntityRecord record;
    record.id = static_cast<std::uint16_t>(track.id);
    record.kind = protocol::EntityKind::kTrack;
    record.state = track.coasting() ? 2 : static_cast<std::uint8_t>(track.status);
    record.flags = protocol::quantize_track_sigma(std::sqrt(track.filter.position_variance()));
    const math::Vec3 estimate_pos = track.position();
    const math::Vec3 estimate_vel = track.velocity();
    record.pos_x = estimate_pos.x;
    record.pos_y = estimate_pos.y;
    record.pos_z = estimate_pos.z;
    record.vel_x = estimate_vel.x;
    record.vel_y = estimate_vel.y;
    record.vel_z = estimate_vel.z;
    output.entities.push_back(record);
  }
}

void SimServer::game_thread_main() {
  // Survival mode: an endless sequence of single-target waves. Each wave is a
  // freshly RESEEDED sim::World, so the deterministic engine is reused verbatim
  // — only the orchestration (lives, scoring, wave variety, the cross-wave
  // monotonic WIRE tick) lives here. No journaling: deterministic replay is the
  // single-engagement path's job (charter §5.8).
  const auto tick_period = nanoseconds(16'666'667);  // 1/60 s.
  // A live target within this range of the ship = a leak. Sized so a hard
  // maneuver CAN push a turn-rate-limited ASM's closest approach outside it
  // (the dodge); a missile homing a stationary ship still strikes dead-on, so
  // not maneuvering is unchanged. (P7+ — was 250 m for the fixed platform.)
  constexpr double kShipHitRangeM = 140.0;
  const auto kDisplayTicks = static_cast<std::uint64_t>(2.0 * sim::kTickRateHz);  // VFX hold.
  const auto kRoundTimeoutTicks = static_cast<std::uint64_t>(45.0 * sim::kTickRateHz);
  const std::uint64_t fire_solution_interval =
      config_.scenario.fire_solution_rate_hz > 0.0
          ? std::max<std::uint64_t>(
                1, static_cast<std::uint64_t>(
                       sim::kTickRateHz / config_.scenario.fire_solution_rate_hz + 0.5))
          : 0;

  // Per-wave world config: reseed the RNG streams + weather and roll K inbound
  // ASMs (count grows with the wave) so each wave is a fresh, escalating
  // multi-target raid. Each ASM drives AT the ship with a small course offset;
  // the terminal phase homes on the ship so an unengaged target reliably hits.
  auto make_cfg = [&](int wave) -> sim::WorldConfig {
    sim::WorldConfig cfg = config_.scenario.config;
    const std::uint64_t spread =
        static_cast<std::uint64_t>(wave) * config_.scenario.game_seed_stride;
    cfg.sim_seed = config_.scenario.config.sim_seed + spread;
    cfg.gust_seed = config_.scenario.config.gust_seed + spread + 0x1234ULL;
    cfg.weather = sim::WeatherGenerator::generate(config_.scenario.weather_seed +
                                                  static_cast<std::uint64_t>(wave));
    // Own ship: a maneuverable frigate. The player steers it (A/D rudder, W/S
    // throttle) to slip the terminal homing of an unengaged ASM — the dodge.
    cfg.ship.initial_position = {0.0, 0.0, 0.0};
    cfg.ship.heading_rad = 0.0;
    cfg.ship.speed_mps = 0.0;
    cfg.ship.max_speed_mps = 20.0;  // ~39 kn flank.
    cfg.ship.accel_mps2 = 2.0;
    cfg.ship.turn_rate_max_rad_s = math::deg_to_rad(10.0);  // Hard-over rate.
    Pcg32 rng(config_.scenario.weather_seed + 0x9E3779B97F4A7C15ULL,
              static_cast<std::uint64_t>(wave) + 1);
    const double ramp = static_cast<double>(std::min(wave - 1, 8));  // 0..8 over waves.
    const int count = std::min(1 + wave / 2, 5);  // 1,2,2,3,3,4,4,5,5 by wave
    auto roll_target = [&](Pcg32& r) -> sim::TargetParams {
      sim::TargetParams t = config_.scenario.config.target;
      const double bearing = r.next_double() * math::kTwoPi;
      const double range_m = 4500.0 + r.next_double() * 3500.0;  // 4.5–8 km out.
      const double alt_m = 150.0 + r.next_double() * 700.0;      // 150–850 m.
      const double east = range_m * math::sin(bearing);
      const double north = range_m * math::cos(bearing);
      t.initial_position = {east, north, alt_m};
      // Cruise dead at the ship's launch point (no jitter): a stationary ship is
      // struck dead-on, but a ship that maneuvers clear before the late terminal
      // pop-up leaves the capped homing unable to recover — the dodge.
      const double inbound = math::atan2(-east, -north);
      t.heading_rad = inbound;
      t.speed_mps = 170.0 + 0.5 * ramp * 30.0 + r.next_double() * 90.0;  // ~170–340.
      t.turn_rate_rad_s = 0.0;
      // Cruise in, pop up near the ship and dive; weave grows with the wave so
      // early raids are catchable and later ones force you to read the maneuver.
      // Late terminal pop-up: the ASM sea-skims dead at the ship's launch point,
      // then pops up and homes only inside ~1 km. A stationary ship is already
      // under the aim → struck; a ship that broke beam-on has opened an offset
      // the capped terminal turn cannot recover in the short dive — the dodge.
      t.popup_range_m = 700.0 + r.next_double() * 300.0;
      t.popup_altitude_m = 240.0;
      t.weave_range_m = wave <= 1 ? 0.0 : (2500.0 + r.next_double() * 1500.0);
      t.weave_turn_rate_rad_s = math::deg_to_rad(4.0 + ramp * 1.2 + r.next_double() * 4.0);
      t.weave_period_s = 3.0 + r.next_double() * 3.0;
      // Terminal turn-rate cap: finite, so a committed own-ship maneuver outruns
      // the homing and forces an overshoot (the dodge). Low enough that a hard
      // turn beats it; grows with the wave so later ASMs are harder to slip.
      t.terminal_turn_rate_max_rad_s = math::deg_to_rad(3.5 + ramp * 0.4);
      return t;
    };
    cfg.target = roll_target(rng);
    cfg.additional_targets.clear();
    for (int j = 1; j < count; ++j) {
      cfg.additional_targets.push_back(roll_target(rng));
    }
    return cfg;
  };

  enum class Phase { kRunning, kDisplay, kGameOver };
  Phase phase = Phase::kRunning;
  int wave = 1;
  int lives = config_.scenario.game_lives;
  std::uint64_t wire_tick = 0;   // Monotonic across ALL waves (delta/ack safe).
  std::uint64_t cadence = 0;
  std::uint64_t phase_ticks = 0;
  std::uint64_t round_ticks = 0;
  std::size_t seen_results = 0, seen_rockets = 0, seen_track_events = 0;
  std::vector<char> resolved;  // per-target: kill/leak already counted+signalled
  bool announce_round = true;  // Emit kRoundStart for the live wave.

  std::optional<sim::World> world;
  world.emplace(make_cfg(wave));
  resolved.assign(world->targets().size(), 0);
  auto next_tick_at = steady_clock::now();

  while (sim_running_.load()) {
    const auto work_started = steady_clock::now();
    SimOutput output;
    const auto step_tick = static_cast<std::uint32_t>(wire_tick);

    if (announce_round) {
      announce_round = false;
      SS_LOG_INFO("game: wave %d begins (lives %d)", wave, lives);
      protocol::EngagementEvent ev;
      ev.tick = step_tick;
      ev.kind = protocol::EventKind::kRoundStart;
      ev.subject_id = static_cast<std::uint16_t>(wave);
      output.events.push_back(ev);
    }

    // 1. Operator commands into the live wave (no journaling here — game mode
    //    reseeds per wave, so replay is the single-engagement path's job).
    SimCommand command;
    while (net_to_sim_.pop(command)) {
      if (command.kind == SimCommand::Kind::kSteer) {
        world->queue_steer(command.steer);  // Helm responds in any phase.
        continue;
      }
      if (phase != Phase::kRunning) {
        continue;  // No live target to shoot between waves.
      }
      sim::FireCommand fire = command.fire;
      if (command.track_id != 0) {
        const auto solution = world->solve_for_track(command.track_id);
        if (!solution.has_value()) {
          stats_.track_solution_failures.fetch_add(1);
          continue;
        }
        fire.azimuth_rad = solution->azimuth_rad + command.fire.azimuth_rad;
        fire.elevation_rad = solution->elevation_rad + command.fire.elevation_rad;
      }
      world->queue_fire(fire);
    }

    // 2. Advance while the wave is live or holding for its result VFX.
    const bool stepping = phase == Phase::kRunning || phase == Phase::kDisplay;
    if (stepping) {
      world->step();
      stats_.ticks.fetch_add(1);
    }

    // 3. Incremental events (launch / resolve / track lifecycle / kill).
    const auto& rockets = world->rockets();
    for (; seen_rockets < rockets.size(); ++seen_rockets) {
      protocol::EngagementEvent ev;
      ev.tick = step_tick;
      ev.kind = protocol::EventKind::kLaunch;
      ev.subject_id = static_cast<std::uint16_t>(rockets[seen_rockets].id);
      output.events.push_back(ev);
    }
    const auto& results = world->results();
    for (; seen_results < results.size(); ++seen_results) {
      const sim::RocketResult& r = results[seen_results];
      protocol::EngagementEvent ev;
      ev.tick = step_tick;
      ev.kind = protocol::EventKind::kRocketResolved;
      ev.subject_id = static_cast<std::uint16_t>(r.rocket_id);
      ev.miss_distance_m = static_cast<float>(r.miss_distance_m);
      ev.detonated = r.detonated;
      ev.killed = r.killed;
      output.events.push_back(ev);
    }
    const auto& track_events = world->track_events();
    for (; seen_track_events < track_events.size(); ++seen_track_events) {
      const sim::TrackEvent& te = track_events[seen_track_events];
      if (te.kind == sim::TrackEvent::Kind::kInitiated) {
        continue;
      }
      protocol::EngagementEvent ev;
      ev.tick = step_tick;
      ev.kind = te.kind == sim::TrackEvent::Kind::kConfirmed ? protocol::EventKind::kTrackConfirmed
                                                             : protocol::EventKind::kTrackLost;
      ev.subject_id = static_cast<std::uint16_t>(te.track_id);
      output.events.push_back(ev);
    }
    // 3b. Per-target KILLS (any time the world is stepping — a rocket can kill
    //     during the result-hold too). subject_id = target id.
    {
      const auto& tgts = world->targets();
      for (std::size_t i = 0; i < tgts.size(); ++i) {
        if (resolved[i] == 0 && tgts[i].destroyed()) {
          resolved[i] = 1;
          SS_LOG_INFO("game: wave %d target %zu DESTROYED (kill)", wave, i);
          protocol::EngagementEvent ev;
          ev.tick = step_tick;
          ev.kind = protocol::EventKind::kTargetDestroyed;
          ev.subject_id = static_cast<std::uint16_t>(i);
          output.events.push_back(ev);
        }
      }
    }

    // 4. Resolution: per-target leak (reached the ship, costs a life), then the
    //    wave ends once ALL targets are resolved (killed or leaked) or it times
    //    out. Leaks only count while the wave is live.
    if (phase == Phase::kRunning) {
      ++round_ticks;
      const auto& tgts = world->targets();
      for (std::size_t i = 0; i < tgts.size(); ++i) {
        if (resolved[i] != 0) {
          continue;
        }
        const math::Vec3& tp = tgts[i].position();
        const math::Vec3 sp = world->ship_position();
        const double leak_dx = tp.x - sp.x;
        const double leak_dy = tp.y - sp.y;
        if (std::sqrt(leak_dx * leak_dx + leak_dy * leak_dy) < kShipHitRangeM) {
          // Always resolve so the wave can progress; only an ARMED enemy attack
          // costs a life and signals the hit. With enemy attack off the ship is
          // invulnerable — the target simply slips past (endless run).
          resolved[i] = 1;
          if (config_.scenario.game_enemy_attack) {
            lives -= 1;
            SS_LOG_INFO("game: wave %d target %zu REACHED SHIP — lives now %d", wave, i, lives);
            protocol::EngagementEvent ev;
            ev.tick = step_tick;
            ev.kind = protocol::EventKind::kTargetHitShip;
            ev.subject_id = static_cast<std::uint16_t>(i);
            output.events.push_back(ev);
          } else {
            SS_LOG_INFO("game: wave %d target %zu slipped past (enemy attack off)", wave, i);
          }
        }
      }
      const bool all_resolved =
          std::all_of(resolved.begin(), resolved.end(), [](char c) { return c != 0; });
      if (all_resolved || round_ticks > kRoundTimeoutTicks) {
        phase = Phase::kDisplay;
        phase_ticks = 0;
      }
    }

    // 5. Snapshot @30Hz while the world is visible.
    if (stepping && cadence % 2 == 0) {
      output.has_snapshot = true;
      output.tick = static_cast<std::uint32_t>(wire_tick);
      output.phase = protocol::EngagementPhase::kRunning;
      append_world_snapshot(output, *world);
    }
    // 5b. Streamed fire solutions for confirmed tracks (PPI lock aid).
    if (phase == Phase::kRunning && fire_solution_interval != 0 &&
        cadence % fire_solution_interval == 0) {
      for (const sim::Track& track : world->tracker().tracks()) {
        if (track.status != sim::TrackStatus::kConfirmed) {
          continue;
        }
        protocol::FireSolution fs;
        fs.tick = static_cast<std::uint32_t>(wire_tick);
        fs.track_id = static_cast<std::uint16_t>(track.id);
        if (const auto sol = world->solve_for_track(track.id)) {
          const math::Vec3 pip = track.position() + track.velocity() * sol->time_of_flight_s;
          fs.valid = true;
          fs.pip_x = pip.x;
          fs.pip_y = pip.y;
          fs.pip_z = pip.z;
          fs.time_of_flight_s = static_cast<float>(sol->time_of_flight_s);
          fs.dispersion_radius_m =
              static_cast<float>(sim::FireCommand{}.dispersion_mrad * 1e-3 * pip.norm());
        }
        output.fire_solutions.push_back(fs);
      }
    }
    ++cadence;
    ++wire_tick;

    // 6. Sub-phase transitions.
    if (phase == Phase::kDisplay && ++phase_ticks >= kDisplayTicks) {
      if (lives <= 0) {
        SS_LOG_INFO("game: GAME OVER at wave %d", wave);
        protocol::EngagementEvent ev;
        ev.tick = static_cast<std::uint32_t>(wire_tick);
        ev.kind = protocol::EventKind::kEngagementEnd;  // Final score card.
        output.events.push_back(ev);
        phase = Phase::kGameOver;
      } else {
        ++wave;
        world.emplace(make_cfg(wave));
        resolved.assign(world->targets().size(), 0);
        seen_results = seen_rockets = seen_track_events = 0;
        round_ticks = 0;
        phase = Phase::kRunning;
        phase_ticks = 0;
        announce_round = true;
      }
    }

    // 7. Publish + wake the I/O thread.
    if (output.has_snapshot || !output.events.empty() || !output.fire_solutions.empty()) {
      if (sim_to_net_.push(std::move(output))) {
        loop_->wakeup();
      } else {
        stats_.sim_output_dropped.fetch_add(1);
      }
    }

    // 8. Tick accounting + fixed-rate pacing (mirrors the single-engagement loop).
    const auto busy_us = static_cast<std::uint64_t>(
        duration_cast<microseconds>(steady_clock::now() - work_started).count());
    stats_.tick_busy_sum_us.fetch_add(busy_us);
    stats_
        .tick_busy_hist[std::min<std::size_t>(SimServerStats::kTickHistBuckets - 1,
                                              std::bit_width(busy_us))]
        .fetch_add(1);
    if (busy_us > stats_.tick_busy_max_us.load()) {
      stats_.tick_busy_max_us.store(busy_us);
    }
    if (busy_us > 8000) {
      stats_.tick_busy_over_8ms.fetch_add(1);
    }
    next_tick_at += tick_period;
    const auto now = steady_clock::now();
    if (now < next_tick_at) {
      std::this_thread::sleep_until(next_tick_at);
    } else if (now - next_tick_at > 10 * tick_period) {
      next_tick_at = now;
    }
  }
}

}  // namespace seashield::server
