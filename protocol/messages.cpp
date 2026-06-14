#include "protocol/messages.h"

#include <algorithm>
#include <cmath>

namespace seashield::protocol {
namespace {

// Enum range guards for untrusted input: decode rejects values outside the
// catalogue instead of laundering them into the type system.
bool valid_role(std::uint8_t v) { return v <= static_cast<std::uint8_t>(Role::kSolo); }
bool valid_reject(std::uint8_t v) { return v <= static_cast<std::uint8_t>(RejectReason::kBadToken); }
bool valid_entity_kind(std::uint8_t v) {
  return v <= static_cast<std::uint8_t>(EntityKind::kOwnShip);
}
bool valid_event_kind(std::uint8_t v) {
  return v <= static_cast<std::uint8_t>(EventKind::kRoundStart);
}
bool valid_phase(std::uint8_t v) { return v <= static_cast<std::uint8_t>(EngagementPhase::kEnded); }

template <typename T>
T quantize_clamped(double value, double lsb, T min, T max) {
  if (!std::isfinite(value)) {
    return 0;  // Defensive: the sim never emits NaN/inf, but the wire must not.
  }
  const double scaled = std::round(value / lsb);
  const double lo = static_cast<double>(min);
  const double hi = static_cast<double>(max);
  return static_cast<T>(std::clamp(scaled, lo, hi));
}

}  // namespace

std::int32_t quantize_position(double meters) {
  return quantize_clamped<std::int32_t>(meters, kPositionLsbM, -kPositionQMax - 1, kPositionQMax);
}

double dequantize_position(std::int32_t q) { return static_cast<double>(q) * kPositionLsbM; }

std::int16_t quantize_velocity(double mps) {
  return quantize_clamped<std::int16_t>(mps, kVelocityLsbMps, INT16_MIN, INT16_MAX);
}

double dequantize_velocity(std::int16_t q) { return static_cast<double>(q) * kVelocityLsbMps; }

std::uint8_t quantize_track_sigma(double sigma_m) {
  if (!(sigma_m > 0.1)) {
    return 0;  // Includes non-finite and degenerate covariances.
  }
  const double q = std::round(50.0 * std::log10(sigma_m / 0.1));
  return static_cast<std::uint8_t>(std::clamp(q, 0.0, 255.0));
}

double dequantize_track_sigma(std::uint8_t q) {
  return 0.1 * std::pow(10.0, static_cast<double>(q) / 50.0);
}

// --- ClientHello -------------------------------------------------------------

void ClientHello::encode(Writer& w) const {
  w.u16(protocol_version);
  w.u8(static_cast<std::uint8_t>(role));
  w.u64(token);
}

std::optional<ClientHello> ClientHello::decode(Reader& r) {
  ClientHello m;
  m.protocol_version = r.u16();
  const std::uint8_t role = r.u8();
  m.token = r.u64();
  if (!r.ok() || !valid_role(role)) {
    return std::nullopt;
  }
  m.role = static_cast<Role>(role);
  return m;
}

// --- ServerWelcome -----------------------------------------------------------

void ServerWelcome::encode(Writer& w) const {
  w.u64(token);
  w.u8(static_cast<std::uint8_t>(role));
  w.u16(udp_port);
  w.u16(tick_rate_hz);
  w.u16(snapshot_rate_hz);
  w.str16(std::string_view(weather_summary).substr(0, 512));
  w.f32(static_cast<float>(surface_wind_east_mps));
  w.f32(static_cast<float>(surface_wind_north_mps));
  w.f32(static_cast<float>(rain_intensity));
  w.f32(static_cast<float>(gust_sigma_mps));
  w.u32(udp_nonce);
}

std::optional<ServerWelcome> ServerWelcome::decode(Reader& r) {
  ServerWelcome m;
  m.token = r.u64();
  const std::uint8_t role = r.u8();
  m.udp_port = r.u16();
  m.tick_rate_hz = r.u16();
  m.snapshot_rate_hz = r.u16();
  m.weather_summary = r.str16();
  m.surface_wind_east_mps = r.f32();
  m.surface_wind_north_mps = r.f32();
  m.rain_intensity = r.f32();
  m.gust_sigma_mps = r.f32();
  m.udp_nonce = r.u32();
  // Negated comparisons so NaN payloads fail closed.
  if (!r.ok() || !valid_role(role) || m.weather_summary.size() > 512 ||
      !(m.rain_intensity >= 0.0 && m.rain_intensity <= 1.0) || !(m.gust_sigma_mps >= 0.0)) {
    return std::nullopt;
  }
  m.role = static_cast<Role>(role);
  return m;
}

// --- ServerReject ------------------------------------------------------------

void ServerReject::encode(Writer& w) const { w.u8(static_cast<std::uint8_t>(reason)); }

std::optional<ServerReject> ServerReject::decode(Reader& r) {
  const std::uint8_t reason = r.u8();
  if (!r.ok() || !valid_reject(reason)) {
    return std::nullopt;
  }
  return ServerReject{static_cast<RejectReason>(reason)};
}

// --- FireRequest -------------------------------------------------------------

void FireRequest::encode(Writer& w) const {
  w.f64(azimuth_rad);
  w.f64(elevation_rad);
  w.u16(salvo_count);
  w.f64(dispersion_mrad);
  w.f64(launch_interval_s);
  w.u16(track_id);
}

std::optional<FireRequest> FireRequest::decode(Reader& r) {
  FireRequest m;
  m.azimuth_rad = r.f64();
  m.elevation_rad = r.f64();
  m.salvo_count = r.u16();
  m.dispersion_mrad = r.f64();
  m.launch_interval_s = r.f64();
  m.track_id = r.u16();
  if (!r.ok()) {
    return std::nullopt;
  }
  return m;
}

// --- ShipCommand -------------------------------------------------------------

void ShipCommand::encode(Writer& w) const {
  w.f64(rudder);
  w.f64(throttle);
}

std::optional<ShipCommand> ShipCommand::decode(Reader& r) {
  ShipCommand m;
  m.rudder = r.f64();
  m.throttle = r.f64();
  if (!r.ok()) {
    return std::nullopt;
  }
  return m;
}

// --- UdpHello / UdpHelloAck / Keepalive ---------------------------------------

void UdpHello::encode(Writer& w) const {
  w.u64(token);
  w.u32(nonce);
}

std::optional<UdpHello> UdpHello::decode(Reader& r) {
  UdpHello m;
  m.token = r.u64();
  m.nonce = r.u32();
  if (!r.ok()) {
    return std::nullopt;
  }
  return m;
}

void EventBacklog::encode(Writer& w) const {
  const std::size_t count = std::min<std::size_t>(events.size(), 255);
  w.u8(static_cast<std::uint8_t>(count));
  for (std::size_t i = 0; i < count; ++i) {
    events[i].encode(w);
  }
}

std::optional<EventBacklog> EventBacklog::decode(Reader& r) {
  EventBacklog m;
  const std::uint8_t count = r.u8();
  m.events.reserve(count);
  for (std::uint8_t i = 0; i < count; ++i) {
    auto event = EngagementEvent::decode(r);
    if (!event.has_value()) {
      return std::nullopt;
    }
    m.events.push_back(*event);
  }
  if (!r.ok()) {
    return std::nullopt;
  }
  return m;
}

std::optional<UdpHelloAck> UdpHelloAck::decode(Reader& r) {
  return r.ok() ? std::optional<UdpHelloAck>(UdpHelloAck{}) : std::nullopt;
}

std::optional<Keepalive> Keepalive::decode(Reader& r) {
  return r.ok() ? std::optional<Keepalive>(Keepalive{}) : std::nullopt;
}

// --- EntityRecord ------------------------------------------------------------

void EntityRecord::encode(Writer& w) const {
  w.u16(id);
  w.u8(static_cast<std::uint8_t>(kind));
  w.u8(state);
  w.u8(flags);
  w.i24(quantize_position(pos_x));
  w.i24(quantize_position(pos_y));
  w.i24(quantize_position(pos_z));
  w.i16(quantize_velocity(vel_x));
  w.i16(quantize_velocity(vel_y));
  w.i16(quantize_velocity(vel_z));
}

std::optional<EntityRecord> EntityRecord::decode(Reader& r) {
  EntityRecord m;
  m.id = r.u16();
  const std::uint8_t kind = r.u8();
  m.state = r.u8();
  m.flags = r.u8();
  m.pos_x = dequantize_position(r.i24());
  m.pos_y = dequantize_position(r.i24());
  m.pos_z = dequantize_position(r.i24());
  m.vel_x = dequantize_velocity(r.i16());
  m.vel_y = dequantize_velocity(r.i16());
  m.vel_z = dequantize_velocity(r.i16());
  if (!r.ok() || !valid_entity_kind(kind)) {
    return std::nullopt;
  }
  m.kind = static_cast<EntityKind>(kind);
  return m;
}

// --- Delta compression (v4) --------------------------------------------------

void SnapshotAck::encode(Writer& w) const { w.u32(tick); }

std::optional<SnapshotAck> SnapshotAck::decode(Reader& r) {
  SnapshotAck m;
  m.tick = r.u32();
  if (!r.ok()) {
    return std::nullopt;
  }
  return m;
}

std::size_t DeltaEntity::encoded_size() const {
  if ((mask & kFullRecord) != 0) {
    return 3 + kEntityRecordBytes;
  }
  return 3 + ((mask & kStateChanged) != 0 ? 1 : 0) + ((mask & kFlagsChanged) != 0 ? 1 : 0) + 6;
}

void DeltaEntity::encode(Writer& w) const {
  w.u8(mask);
  w.u16(id);
  if ((mask & kFullRecord) != 0) {
    full.encode(w);
    return;
  }
  if ((mask & kStateChanged) != 0) {
    w.u8(state);
  }
  if ((mask & kFlagsChanged) != 0) {
    w.u8(flags);
  }
  for (const std::int8_t v : res_pos) {
    w.u8(static_cast<std::uint8_t>(v));
  }
  for (const std::int8_t v : res_vel) {
    w.u8(static_cast<std::uint8_t>(v));
  }
}

std::optional<DeltaEntity> DeltaEntity::decode(Reader& r) {
  DeltaEntity m;
  m.mask = r.u8();
  m.id = r.u16();
  if ((m.mask & ~(kStateChanged | kFlagsChanged | kFullRecord |
                  (0x3u << kKindShift))) != 0 ||
      !valid_entity_kind(static_cast<std::uint8_t>((m.mask >> kKindShift) & 0x3))) {
    return std::nullopt;  // Reserved mask bits / unknown kind: strict reject.
  }
  if ((m.mask & kFullRecord) != 0) {
    const auto full = EntityRecord::decode(r);
    if (!full.has_value()) {
      return std::nullopt;
    }
    m.full = *full;
    return m;
  }
  if ((m.mask & kStateChanged) != 0) {
    m.state = r.u8();
  }
  if ((m.mask & kFlagsChanged) != 0) {
    m.flags = r.u8();
  }
  for (std::int8_t& v : m.res_pos) {
    v = static_cast<std::int8_t>(r.u8());
  }
  for (std::int8_t& v : m.res_vel) {
    v = static_cast<std::int8_t>(r.u8());
  }
  if (!r.ok()) {
    return std::nullopt;
  }
  return m;
}

namespace {

bool fits_i8(std::int64_t v) { return v >= INT8_MIN && v <= INT8_MAX; }

}  // namespace

DeltaEntity make_delta_entity(const EntityRecord& base, const EntityRecord& current,
                              std::uint32_t dticks, std::uint16_t tick_rate_hz) {
  DeltaEntity delta;
  delta.id = current.id;
  delta.mask = static_cast<std::uint8_t>(static_cast<std::uint8_t>(current.kind)
                                         << DeltaEntity::kKindShift);

  const std::int32_t base_pos_q[3] = {quantize_position(base.pos_x), quantize_position(base.pos_y),
                                      quantize_position(base.pos_z)};
  const std::int16_t base_vel_q[3] = {quantize_velocity(base.vel_x), quantize_velocity(base.vel_y),
                                      quantize_velocity(base.vel_z)};
  const std::int32_t cur_pos_q[3] = {quantize_position(current.pos_x),
                                     quantize_position(current.pos_y),
                                     quantize_position(current.pos_z)};
  const std::int16_t cur_vel_q[3] = {quantize_velocity(current.vel_x),
                                     quantize_velocity(current.vel_y),
                                     quantize_velocity(current.vel_z)};

  std::int64_t res_pos[3];
  std::int64_t res_vel[3];
  bool fits = base.kind == current.kind;
  for (int axis = 0; axis < 3 && fits; ++axis) {
    res_pos[axis] = static_cast<std::int64_t>(cur_pos_q[axis]) -
                    predict_position_q(base_pos_q[axis], base_vel_q[axis], dticks, tick_rate_hz);
    res_vel[axis] = static_cast<std::int64_t>(cur_vel_q[axis]) - base_vel_q[axis];
    fits = fits_i8(res_pos[axis]) && fits_i8(res_vel[axis]);
  }
  if (!fits) {
    delta.mask |= DeltaEntity::kFullRecord;
    delta.full = current;
    return delta;
  }
  for (int axis = 0; axis < 3; ++axis) {
    delta.res_pos[axis] = static_cast<std::int8_t>(res_pos[axis]);
    delta.res_vel[axis] = static_cast<std::int8_t>(res_vel[axis]);
  }
  if (current.state != base.state) {
    delta.mask |= DeltaEntity::kStateChanged;
    delta.state = current.state;
  }
  if (current.flags != base.flags) {
    delta.mask |= DeltaEntity::kFlagsChanged;
    delta.flags = current.flags;
  }
  return delta;
}

EntityRecord apply_delta_entity(const EntityRecord& base, const DeltaEntity& delta,
                                std::uint32_t dticks, std::uint16_t tick_rate_hz) {
  if ((delta.mask & DeltaEntity::kFullRecord) != 0) {
    return delta.full;
  }
  EntityRecord out = base;
  out.id = delta.id;
  for (int axis = 0; axis < 3; ++axis) {
    const std::int32_t base_pos_q = quantize_position(axis == 0   ? base.pos_x
                                                      : axis == 1 ? base.pos_y
                                                                  : base.pos_z);
    const std::int16_t base_vel_q = quantize_velocity(axis == 0   ? base.vel_x
                                                      : axis == 1 ? base.vel_y
                                                                  : base.vel_z);
    const double pos = dequantize_position(
        predict_position_q(base_pos_q, base_vel_q, dticks, tick_rate_hz) + delta.res_pos[axis]);
    const double vel = dequantize_velocity(
        static_cast<std::int16_t>(base_vel_q + delta.res_vel[axis]));
    (axis == 0 ? out.pos_x : axis == 1 ? out.pos_y : out.pos_z) = pos;
    (axis == 0 ? out.vel_x : axis == 1 ? out.vel_y : out.vel_z) = vel;
  }
  if ((delta.mask & DeltaEntity::kStateChanged) != 0) {
    out.state = delta.state;
  }
  if ((delta.mask & DeltaEntity::kFlagsChanged) != 0) {
    out.flags = delta.flags;
  }
  return out;
}

void SnapshotDelta::encode(Writer& w) const {
  w.u32(tick);
  w.u32(base_tick);
  w.u8(static_cast<std::uint8_t>(phase));
  w.u16(total_entities);
  w.u16(first_index);
  const std::size_t count = std::min<std::size_t>(entities.size(), 255);
  w.u8(static_cast<std::uint8_t>(count));
  for (std::size_t i = 0; i < count; ++i) {
    entities[i].encode(w);
  }
}

std::optional<SnapshotDelta> SnapshotDelta::decode(Reader& r) {
  SnapshotDelta m;
  m.tick = r.u32();
  m.base_tick = r.u32();
  const std::uint8_t phase = r.u8();
  m.total_entities = r.u16();
  m.first_index = r.u16();
  const std::uint8_t count = r.u8();
  if (!r.ok() || !valid_phase(phase) || m.base_tick >= m.tick ||
      static_cast<std::size_t>(m.first_index) + count > m.total_entities) {
    return std::nullopt;
  }
  m.phase = static_cast<EngagementPhase>(phase);
  m.entities.reserve(count);
  for (std::uint8_t i = 0; i < count; ++i) {
    auto entity = DeltaEntity::decode(r);
    if (!entity.has_value()) {
      return std::nullopt;
    }
    m.entities.push_back(std::move(*entity));
  }
  return m;
}

// --- Snapshot ----------------------------------------------------------------

void Snapshot::encode(Writer& w) const {
  w.u32(tick);
  w.u8(static_cast<std::uint8_t>(phase));
  w.u16(total_entities);
  w.u16(first_index);
  w.u8(static_cast<std::uint8_t>(std::min<std::size_t>(entities.size(), 255)));
  const std::size_t count = std::min<std::size_t>(entities.size(), 255);
  for (std::size_t i = 0; i < count; ++i) {
    entities[i].encode(w);
  }
}

std::optional<Snapshot> Snapshot::decode(Reader& r) {
  Snapshot m;
  m.tick = r.u32();
  const std::uint8_t phase = r.u8();
  m.total_entities = r.u16();
  m.first_index = r.u16();
  const std::uint8_t count = r.u8();
  if (!r.ok() || !valid_phase(phase)) {
    return std::nullopt;
  }
  m.phase = static_cast<EngagementPhase>(phase);
  // Batch consistency: the slice [first_index, first_index+count) must lie
  // inside [0, total) — rejects truncated or internally inconsistent batches.
  if (static_cast<std::uint32_t>(m.first_index) + count > m.total_entities) {
    return std::nullopt;
  }
  m.entities.reserve(count);
  for (std::uint8_t i = 0; i < count; ++i) {
    auto entity = EntityRecord::decode(r);
    if (!entity) {
      return std::nullopt;
    }
    m.entities.push_back(*entity);
  }
  return m;
}

// --- EngagementEvent ----------------------------------------------------------

void EngagementEvent::encode(Writer& w) const {
  w.u32(tick);
  w.u8(static_cast<std::uint8_t>(kind));
  w.u16(subject_id);
  w.f32(miss_distance_m);
  w.u8(detonated ? 1 : 0);
  w.u8(killed ? 1 : 0);
}

std::optional<EngagementEvent> EngagementEvent::decode(Reader& r) {
  EngagementEvent m;
  m.tick = r.u32();
  const std::uint8_t kind = r.u8();
  m.subject_id = r.u16();
  m.miss_distance_m = r.f32();
  const std::uint8_t detonated = r.u8();
  const std::uint8_t killed = r.u8();
  if (!r.ok() || !valid_event_kind(kind) || detonated > 1 || killed > 1) {
    return std::nullopt;
  }
  m.kind = static_cast<EventKind>(kind);
  m.detonated = detonated != 0;
  m.killed = killed != 0;
  return m;
}

// --- FireSolution -------------------------------------------------------------

void FireSolution::encode(Writer& w) const {
  w.u32(tick);
  w.u16(track_id);
  w.u8(valid ? 1 : 0);
  w.i24(quantize_position(pip_x));
  w.i24(quantize_position(pip_y));
  w.i24(quantize_position(pip_z));
  w.f32(time_of_flight_s);
  w.f32(dispersion_radius_m);
}

std::optional<FireSolution> FireSolution::decode(Reader& r) {
  FireSolution m;
  m.tick = r.u32();
  m.track_id = r.u16();
  const std::uint8_t valid = r.u8();
  m.pip_x = dequantize_position(r.i24());
  m.pip_y = dequantize_position(r.i24());
  m.pip_z = dequantize_position(r.i24());
  m.time_of_flight_s = r.f32();
  m.dispersion_radius_m = r.f32();
  if (!r.ok() || valid > 1) {
    return std::nullopt;
  }
  m.valid = valid != 0;
  return m;
}

// --- Envelopes ----------------------------------------------------------------

namespace {

template <typename M>
std::optional<ControlMessage> decode_control_as(Reader& r) {
  auto msg = M::decode(r);
  if (!msg || !r.finished()) {
    return std::nullopt;
  }
  return ControlMessage(*msg);
}

template <typename M>
std::optional<DataMessage> decode_data_as(Reader& r) {
  auto msg = M::decode(r);
  if (!msg || !r.finished()) {
    return std::nullopt;
  }
  return DataMessage(*msg);
}

}  // namespace

std::optional<ControlMessage> decode_control_frame(std::span<const std::uint8_t> frame) {
  Reader r(frame);
  const std::uint8_t type = r.u8();
  if (!r.ok()) {
    return std::nullopt;
  }
  switch (static_cast<MsgType>(type)) {
    case MsgType::kClientHello:
      return decode_control_as<ClientHello>(r);
    case MsgType::kServerWelcome:
      return decode_control_as<ServerWelcome>(r);
    case MsgType::kServerReject:
      return decode_control_as<ServerReject>(r);
    case MsgType::kFireRequest:
      return decode_control_as<FireRequest>(r);
    case MsgType::kEventBacklog:
      return decode_control_as<EventBacklog>(r);
    case MsgType::kShipCommand:
      return decode_control_as<ShipCommand>(r);
    default:
      return std::nullopt;
  }
}

std::optional<DataMessage> decode_data_message(MsgType type, std::span<const std::uint8_t> payload) {
  Reader r(payload);
  switch (type) {
    case MsgType::kUdpHello:
      return decode_data_as<UdpHello>(r);
    case MsgType::kUdpHelloAck:
      return decode_data_as<UdpHelloAck>(r);
    case MsgType::kKeepalive:
      return decode_data_as<Keepalive>(r);
    case MsgType::kSnapshot:
      return decode_data_as<Snapshot>(r);
    case MsgType::kEngagementEvent:
      return decode_data_as<EngagementEvent>(r);
    case MsgType::kFireSolution:
      return decode_data_as<FireSolution>(r);
    case MsgType::kSnapshotAck:
      return decode_data_as<SnapshotAck>(r);
    case MsgType::kSnapshotDelta:
      return decode_data_as<SnapshotDelta>(r);
    default:
      return std::nullopt;
  }
}

}  // namespace seashield::protocol
