#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "protocol/wire.h"

// Message catalogue for P3 (charter §6, protocol-spec.md). Two transports:
//
//   TCP  (control)  — framed [type u8][payload]; loss/ordering handled by TCP.
//   UDP  (data)     — messages ride inside packets built by ReliableEndpoint
//                     (protocol/reliable.h) on either the Unreliable or the
//                     Reliable-Unordered channel.
//
// Channel assignment: Snapshot/UdpHello/UdpHelloAck/Keepalive are unreliable
// (latest state wins, retransmitting stale state is worthless); the
// EngagementEvent is Reliable-Unordered — it carries its own tick, so the
// receiver can reconstruct order without an ordered channel (charter §5.7).
namespace seashield::protocol {

inline constexpr std::uint16_t kProtocolVersion = 1;

// Hard ceiling for one UDP datagram including the 12-byte packet header
// (charter §6 "단편화 정책"): snapshots above this split into independently
// decodable batches instead of relying on IP fragmentation.
inline constexpr std::size_t kMaxDatagramBytes = 1200;

enum class MsgType : std::uint8_t {
  // TCP control channel.
  kClientHello = 1,
  kServerWelcome = 2,
  kServerReject = 3,
  kFireRequest = 4,
  // UDP data channels.
  kUdpHello = 16,
  kUdpHelloAck = 17,
  kKeepalive = 18,
  kSnapshot = 19,
  kEngagementEvent = 20,
};

// Console roles (charter §3.2). kSolo is the single-client mode that holds
// every permission at once — it keeps a one-machine demo viable.
enum class Role : std::uint8_t {
  kObserver = 0,
  kCommander = 1,
  kWeapons = 2,
  kSolo = 3,
};

enum class RejectReason : std::uint8_t {
  kVersionMismatch = 0,
  kRoleTaken = 1,
  kServerFull = 2,
  kBadToken = 3,
};

enum class EntityKind : std::uint8_t {
  kTarget = 0,
  kRocket = 1,
};

enum class EventKind : std::uint8_t {
  kLaunch = 0,
  kRocketResolved = 1,
  kTargetDestroyed = 2,
  kEngagementEnd = 3,
};

enum class EngagementPhase : std::uint8_t {
  kRunning = 0,
  kEnded = 1,
};

// --- Quantization (charter §6 "양자화") -----------------------------------
//
// Positions: 1 cm LSB in a signed 24-bit field -> ±83.9 km, comfortably
// covering the 40 km × 40 km × 10 km scenario box (charter §5.2).
// Velocities: 0.1 m/s LSB in a signed 16-bit field -> ±3276.7 m/s, above any
// subsonic threat or boost-phase rocket in scope.
// Out-of-range and non-finite values clamp; quantization error is bounded by
// LSB/2 and verified in tests.

inline constexpr double kPositionLsbM = 0.01;
inline constexpr double kVelocityLsbMps = 0.1;
inline constexpr std::int32_t kPositionQMax = (1 << 23) - 1;

std::int32_t quantize_position(double meters);
double dequantize_position(std::int32_t q);
std::int16_t quantize_velocity(double mps);
double dequantize_velocity(std::int16_t q);

// --- TCP control messages ---------------------------------------------------

struct ClientHello {
  static constexpr MsgType kType = MsgType::kClientHello;
  std::uint16_t protocol_version = kProtocolVersion;
  Role role = Role::kObserver;
  std::uint64_t token = 0;  // 0 = new session; nonzero = reconnect (charter §4.8).

  void encode(Writer& w) const;
  static std::optional<ClientHello> decode(Reader& r);
};

struct ServerWelcome {
  static constexpr MsgType kType = MsgType::kServerWelcome;
  std::uint64_t token = 0;
  Role role = Role::kObserver;
  std::uint16_t udp_port = 0;
  std::uint16_t tick_rate_hz = 60;
  std::uint16_t snapshot_rate_hz = 30;
  std::string weather_summary;  // Human-readable, display only (≤512 bytes).

  void encode(Writer& w) const;
  static std::optional<ServerWelcome> decode(Reader& r);
};

struct ServerReject {
  static constexpr MsgType kType = MsgType::kServerReject;
  RejectReason reason = RejectReason::kVersionMismatch;

  void encode(Writer& w) const;
  static std::optional<ServerReject> decode(Reader& r);
};

// Mirrors sim::FireCommand field-for-field; the server revalidates ranges
// before the command crosses into the simulation thread.
struct FireRequest {
  static constexpr MsgType kType = MsgType::kFireRequest;
  double azimuth_rad = 0.0;
  double elevation_rad = 0.0;
  std::uint16_t salvo_count = 1;
  double dispersion_mrad = 5.0;
  double launch_interval_s = 0.05;

  void encode(Writer& w) const;
  static std::optional<FireRequest> decode(Reader& r);
};

// --- UDP data messages ------------------------------------------------------

// Repeated by the client until UdpHelloAck arrives: binds the client's UDP
// source address to its logical session via the token (charter §6 세션 흐름).
struct UdpHello {
  static constexpr MsgType kType = MsgType::kUdpHello;
  std::uint64_t token = 0;

  void encode(Writer& w) const;
  static std::optional<UdpHello> decode(Reader& r);
};

struct UdpHelloAck {
  static constexpr MsgType kType = MsgType::kUdpHelloAck;

  void encode(Writer&) const {}
  static std::optional<UdpHelloAck> decode(Reader& r);
};

// Periodic client->server heartbeat. Its real cargo is the packet header it
// rides in: the acks that confirm reliable events and feed RTT estimation.
struct Keepalive {
  static constexpr MsgType kType = MsgType::kKeepalive;

  void encode(Writer&) const {}
  static std::optional<Keepalive> decode(Reader& r);
};

// One simulated entity, 20 bytes on the wire (charter §6 산정: id/type/state
// 5B + position 9B + velocity 6B). Stored dequantized on the struct so both
// sides handle plain doubles.
struct EntityRecord {
  std::uint16_t id = 0;
  EntityKind kind = EntityKind::kTarget;
  std::uint8_t state = 0;  // Target: 0 alive, 1 destroyed. Rocket: 0 boost, 1 glide.
  std::uint8_t flags = 0;  // Reserved.
  double pos_x = 0.0, pos_y = 0.0, pos_z = 0.0;
  double vel_x = 0.0, vel_y = 0.0, vel_z = 0.0;

  void encode(Writer& w) const;
  static std::optional<EntityRecord> decode(Reader& r);
};

inline constexpr std::size_t kEntityRecordBytes = 20;

// Full-snapshot batch (charter §6): one datagram's worth of entities plus
// enough context (tick, total, first_index) to be useful on its own — a lost
// sibling batch never blocks decoding this one.
struct Snapshot {
  static constexpr MsgType kType = MsgType::kSnapshot;
  std::uint32_t tick = 0;
  EngagementPhase phase = EngagementPhase::kRunning;
  std::uint16_t total_entities = 0;
  std::uint16_t first_index = 0;
  std::vector<EntityRecord> entities;  // ≤255 per batch (count travels as u8).

  void encode(Writer& w) const;
  static std::optional<Snapshot> decode(Reader& r);
};

// Engagement adjudication event (charter §5.7), Reliable-Unordered: must
// arrive exactly once, while the embedded tick restores ordering.
struct EngagementEvent {
  static constexpr MsgType kType = MsgType::kEngagementEvent;
  std::uint32_t tick = 0;
  EventKind kind = EventKind::kLaunch;
  std::uint16_t rocket_id = 0;  // 0 for target/engagement-level events.
  float miss_distance_m = 0.0F;
  bool detonated = false;
  bool killed = false;

  void encode(Writer& w) const;
  static std::optional<EngagementEvent> decode(Reader& r);
};

// --- Envelopes --------------------------------------------------------------

using ControlMessage = std::variant<ClientHello, ServerWelcome, ServerReject, FireRequest>;
using DataMessage = std::variant<UdpHello, UdpHelloAck, Keepalive, Snapshot, EngagementEvent>;

// TCP frame body: [type u8][payload]. The length-prefix framing itself is
// net::FrameParser's job (design doc §5).
template <typename M>
std::vector<std::uint8_t> encode_control_frame(const M& msg) {
  Writer w;
  w.u8(static_cast<std::uint8_t>(M::kType));
  msg.encode(w);
  return w.take();
}

// Bare message payload, for handing to ReliableEndpoint with M::kType.
template <typename M>
std::vector<std::uint8_t> encode_payload(const M& msg) {
  Writer w;
  msg.encode(w);
  return w.take();
}

// Strict decoders: unknown type, short read, or trailing bytes -> nullopt.
std::optional<ControlMessage> decode_control_frame(std::span<const std::uint8_t> frame);
std::optional<DataMessage> decode_data_message(MsgType type, std::span<const std::uint8_t> payload);

}  // namespace seashield::protocol
