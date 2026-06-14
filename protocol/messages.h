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

// v2 (P4): kTrack entities, track lifecycle events, FireRequest.track_id,
// FireSolution. Strict decoders silently reject unknown enums, so mixed
// versions would "half work" (no tracks visible) — bumping forces the loud
// failure instead. v1->v2 history: docs/architecture/protocol-spec.md.
// v3 (P5): ServerWelcome carries visual-driver weather scalars and the
// server streams FireSolution for confirmed tracks (low cadence).
// v4 (P6): delta-compressed snapshots (SnapshotDelta) against the client's
// acked baseline (SnapshotAck), plus the reconnect event backlog over TCP.
inline constexpr std::uint16_t kProtocolVersion = 4;

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
  kEventBacklog = 5,  // Server -> client catch-up at UDP-bind time (v4).
  // UDP data channels.
  kUdpHello = 16,
  kUdpHelloAck = 17,
  kKeepalive = 18,
  kSnapshot = 19,
  kEngagementEvent = 20,
  kFireSolution = 21,
  kSnapshotAck = 22,    // Client -> server: newest fully-assembled tick.
  kSnapshotDelta = 23,  // Server -> client: residuals vs the acked baseline.
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
  kTrack = 2,  // Kalman ESTIMATE of a target — what the consoles actually see.
};

enum class EventKind : std::uint8_t {
  kLaunch = 0,
  kRocketResolved = 1,
  kTargetDestroyed = 2,
  kEngagementEnd = 3,
  // Track lifecycle (P4). Initiation has no event on purpose: tentative
  // tracks are frequent and short-lived, and the snapshot's state byte
  // already shows them — only the transitions a console must not miss ride
  // the reliable channel (charter §4.3 채널 배정 철학).
  kTrackConfirmed = 4,
  kTrackLost = 5,
  // Survival game mode (P7 pl=playable track). subject_id carries the wave
  // index for kRoundStart and is 0 for kTargetHitShip. Both ride the same
  // reliable channel as the other adjudication events; the console derives
  // score/lives/wave from the (kTargetDestroyed, kTargetHitShip, kRoundStart)
  // event stream — no separate scoreboard message is needed.
  kTargetHitShip = 6,  // A live target reached the ship: the operator lost a life.
  kRoundStart = 7,     // A new wave began; subject_id = wave number (1-based).
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

// Track quality (position σ, meters) in the EntityRecord flags byte:
// logarithmic, q = 50·log10(σ/0.1 m), covering 0.1 m..12.6 km at ~4.7% per
// step — plenty for drawing an uncertainty ellipse on the PPI.
std::uint8_t quantize_track_sigma(double sigma_m);
double dequantize_track_sigma(std::uint8_t q);

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
  // Visual-driver weather scalars (v3, P5 client): sea state, rain and trail
  // bend come from the same simulation weather, not from client guesswork.
  // The full generator stays server-side; these are display inputs only.
  double surface_wind_east_mps = 0.0;
  double surface_wind_north_mps = 0.0;
  double rain_intensity = 0.0;  // 0..1, same scale the radar attenuation uses.
  double gust_sigma_mps = 0.0;  // OU turbulence σ near the surface.
  // Per-incarnation UDP binding nonce (v4) — the client echoes it in
  // UdpHello; see UdpHello::nonce.
  std::uint32_t udp_nonce = 0;

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
  // 0 = manual fire at the absolute az/el above. Nonzero = fire at this
  // track's server-computed solution, with az/el reinterpreted as OPERATOR
  // OFFSETS on top of it (charter §5.6 항목 5 운용자 보정).
  std::uint16_t track_id = 0;

  void encode(Writer& w) const;
  static std::optional<FireRequest> decode(Reader& r);
};

// --- UDP data messages ------------------------------------------------------

// Repeated by the client until UdpHelloAck arrives: binds the client's UDP
// source address to its logical session via the token (charter §6 세션 흐름).
struct UdpHello {
  static constexpr MsgType kType = MsgType::kUdpHello;
  std::uint64_t token = 0;
  // Echo of ServerWelcome::udp_nonce (v4): binds the hello to THIS transport
  // incarnation, so a stale hello from a pre-reconnect socket can never
  // steal the fresh session's UDP binding.
  std::uint32_t nonce = 0;

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
  // Target: 0 alive, 1 destroyed. Rocket: 0 boost, 1 glide.
  // Track: 0 tentative, 1 confirmed, 2 coasting (confirmed, missing scans).
  std::uint8_t state = 0;
  // Track: quantize_track_sigma(position σ). Other kinds: reserved (0).
  std::uint8_t flags = 0;
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

// --- Delta compression (v4, charter §6 "델타 압축") -------------------------
//
// Nearly every entity moves every tick, so "send only changed entities" saves
// nothing here. Instead both ends run the SAME integer CV prediction from the
// client's last ACKED snapshot and only the residual travels (~9B vs 20B).
// Deltas always reference a baseline the client provably has, so a lost delta
// never breaks the chain — the next one still decodes (acked-baseline scheme).

// Quantizers shared with EntityRecord's codec — the residual math must run in
// quantized units on both ends or the prediction would not be bit-identical.
std::int32_t quantize_position(double meters);
double dequantize_position(std::int32_t q);
std::int16_t quantize_velocity(double mps);
double dequantize_velocity(std::int16_t q);

// Integer-exact CV prediction: positions in quantized cm, velocities in
// quantized 0.1 m/s (= 10 cm/s). 64-bit multiply first, ONE truncating
// division last — encoder and decoder agree by construction.
constexpr std::int32_t predict_position_q(std::int32_t base_pos_q, std::int16_t base_vel_q,
                                          std::uint32_t dticks, std::uint16_t tick_rate_hz) {
  return base_pos_q +
         static_cast<std::int32_t>((static_cast<std::int64_t>(base_vel_q) * 10 *
                                    static_cast<std::int64_t>(dticks)) /
                                   tick_rate_hz);
}

// Client -> server: the newest fully-assembled snapshot tick, i.e. the
// baseline the server may delta against.
struct SnapshotAck {
  static constexpr MsgType kType = MsgType::kSnapshotAck;
  std::uint32_t tick = 0;

  void encode(Writer& w) const;
  static std::optional<SnapshotAck> decode(Reader& r);
};

// One entity inside a delta batch: a 9-byte residual against the same
// (kind, id) entity in the baseline, or a full EntityRecord escape (new
// entity, residual overflow, a track's update-tick jump). kind rides in mask
// bits 3-4 so the residual shape stays at 9 bytes.
struct DeltaEntity {
  static constexpr std::uint8_t kStateChanged = 1u << 0;
  static constexpr std::uint8_t kFlagsChanged = 1u << 1;
  static constexpr std::uint8_t kFullRecord = 1u << 2;
  static constexpr std::uint8_t kKindShift = 3;

  std::uint8_t mask = 0;
  std::uint16_t id = 0;
  std::uint8_t state = 0;  // Valid when kStateChanged; else inherit the base.
  std::uint8_t flags = 0;  // Valid when kFlagsChanged.
  std::int8_t res_pos[3] = {0, 0, 0};  // Residual vs prediction, quantized cm.
  std::int8_t res_vel[3] = {0, 0, 0};  // Residual vs base, quantized 0.1 m/s.
  EntityRecord full;                   // Valid when kFullRecord.

  EntityKind kind() const { return static_cast<EntityKind>((mask >> kKindShift) & 0x3); }
  std::size_t encoded_size() const;
  void encode(Writer& w) const;
  static std::optional<DeltaEntity> decode(Reader& r);
};

// Encoder/decoder pair — the only two places that know the residual math,
// kept side by side so they cannot drift (the round trip is pinned by tests).
// make_ falls back to a full-escape entity whenever a residual misses i8.
DeltaEntity make_delta_entity(const EntityRecord& base, const EntityRecord& current,
                              std::uint32_t dticks, std::uint16_t tick_rate_hz);
EntityRecord apply_delta_entity(const EntityRecord& base, const DeltaEntity& delta,
                                std::uint32_t dticks, std::uint16_t tick_rate_hz);

// Delta batch: same self-describing batching contract as Snapshot (a lost
// sibling never blocks this one). Entities ABSENT from the delta no longer
// exist — removal is implicit because every live entity is listed.
struct SnapshotDelta {
  static constexpr MsgType kType = MsgType::kSnapshotDelta;
  std::uint32_t tick = 0;
  std::uint32_t base_tick = 0;
  EngagementPhase phase = EngagementPhase::kRunning;
  std::uint16_t total_entities = 0;
  std::uint16_t first_index = 0;
  std::vector<DeltaEntity> entities;  // ≤255 per batch (count travels as u8).

  void encode(Writer& w) const;
  static std::optional<SnapshotDelta> decode(Reader& r);
};

// Engagement adjudication event (charter §5.7), Reliable-Unordered: must
// arrive exactly once, while the embedded tick restores ordering.
struct EngagementEvent {
  static constexpr MsgType kType = MsgType::kEngagementEvent;
  std::uint32_t tick = 0;
  EventKind kind = EventKind::kLaunch;
  // Rocket id for launch/resolve, track id for track events, 0 otherwise
  // (renamed from rocket_id in v2; same wire bytes).
  std::uint16_t subject_id = 0;
  float miss_distance_m = 0.0F;
  bool detonated = false;
  bool killed = false;

  void encode(Writer& w) const;
  static std::optional<EngagementEvent> decode(Reader& r);
};

// Server-computed fire solution for a designated track (charter §4.3 "PIP/
// 예상 산포 갱신" — unreliable UDP). Defined and tested in P4; the server
// starts streaming it when the weapons console UI exists (P5).
struct FireSolution {
  static constexpr MsgType kType = MsgType::kFireSolution;
  std::uint32_t tick = 0;
  std::uint16_t track_id = 0;
  bool valid = false;  // False = solver did not converge for this track.
  double pip_x = 0.0, pip_y = 0.0, pip_z = 0.0;  // Quantized like positions.
  float time_of_flight_s = 0.0F;
  float dispersion_radius_m = 0.0F;  // Expected 1σ pattern radius at the PIP.

  void encode(Writer& w) const;
  static std::optional<FireSolution> decode(Reader& r);
};

// Reconnect/late-join catch-up (v4, charter §5.8 AAR): every engagement
// event the session has not yet been shown, replayed over TCP at UDP-bind
// time. The live reliable-UDP channel takes over from there; the client's
// (kind, subject, tick) dedup absorbs any boundary overlap.
struct EventBacklog {
  static constexpr MsgType kType = MsgType::kEventBacklog;
  std::vector<EngagementEvent> events;  // ≤255 per frame (count travels as u8).

  void encode(Writer& w) const;
  static std::optional<EventBacklog> decode(Reader& r);
};

// --- Envelopes --------------------------------------------------------------

using ControlMessage =
    std::variant<ClientHello, ServerWelcome, ServerReject, FireRequest, EventBacklog>;
using DataMessage =
    std::variant<UdpHello, UdpHelloAck, Keepalive, Snapshot, EngagementEvent, FireSolution,
                 SnapshotAck, SnapshotDelta>;

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
