#include "protocol/messages.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace seashield::protocol {
namespace {

template <typename M>
std::optional<ControlMessage> control_round_trip(const M& msg) {
  return decode_control_frame(encode_control_frame(msg));
}

template <typename M>
std::optional<DataMessage> data_round_trip(const M& msg) {
  return decode_data_message(M::kType, encode_payload(msg));
}

TEST(MessagesTest, ClientHelloRoundTrip) {
  ClientHello hello;
  hello.protocol_version = kProtocolVersion;
  hello.role = Role::kWeapons;
  hello.token = 0xFEEDFACECAFEBEEFull;

  const auto decoded = control_round_trip(hello);
  ASSERT_TRUE(decoded.has_value());
  const auto* m = std::get_if<ClientHello>(&*decoded);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->protocol_version, kProtocolVersion);
  EXPECT_EQ(m->role, Role::kWeapons);
  EXPECT_EQ(m->token, 0xFEEDFACECAFEBEEFull);
}

TEST(MessagesTest, ServerWelcomeRoundTripWithWeatherSummary) {
  ServerWelcome welcome;
  welcome.token = 42;
  welcome.role = Role::kSolo;
  welcome.udp_port = 7778;
  welcome.weather_summary = "wind 8.2 m/s from 240°, rain 0.3";
  welcome.surface_wind_east_mps = -7.1;
  welcome.surface_wind_north_mps = 4.25;
  welcome.rain_intensity = 0.3;
  welcome.gust_sigma_mps = 1.5;

  const auto decoded = control_round_trip(welcome);
  ASSERT_TRUE(decoded.has_value());
  const auto* m = std::get_if<ServerWelcome>(&*decoded);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->token, 42u);
  EXPECT_EQ(m->role, Role::kSolo);
  EXPECT_EQ(m->udp_port, 7778);
  EXPECT_EQ(m->tick_rate_hz, 60);
  EXPECT_EQ(m->snapshot_rate_hz, 30);
  EXPECT_EQ(m->weather_summary, "wind 8.2 m/s from 240°, rain 0.3");
  // v3 visual-driver scalars travel as f32: exact at these literals.
  EXPECT_DOUBLE_EQ(m->surface_wind_east_mps, -7.1F);
  EXPECT_DOUBLE_EQ(m->surface_wind_north_mps, 4.25);
  EXPECT_DOUBLE_EQ(m->rain_intensity, 0.3F);
  EXPECT_DOUBLE_EQ(m->gust_sigma_mps, 1.5);
}

TEST(MessagesTest, SnapshotAckRoundTrip) {
  SnapshotAck ack;
  ack.tick = 0xDEADBEEF;
  Writer w;
  ack.encode(w);
  Reader r(w.data());
  const auto decoded = SnapshotAck::decode(r);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->tick, 0xDEADBEEFu);
}

TEST(MessagesTest, PredictPositionIsIntegerExactWithTruncation) {
  // vel_q is 0.1 m/s = 10 cm/s; over dticks at 60 Hz the advance is
  // (vel_q*10*dticks)/60 cm with ONE truncating division at the end.
  EXPECT_EQ(predict_position_q(1000, 60, 2, 60), 1000 + 20);   // 6 m/s * 2 ticks.
  EXPECT_EQ(predict_position_q(1000, -60, 2, 60), 1000 - 20);  // Symmetric for negatives.
  EXPECT_EQ(predict_position_q(0, 1, 1, 60), 0);    // 10*1/60 truncates to 0.
  EXPECT_EQ(predict_position_q(0, -1, 1, 60), 0);   // -10/60 truncates toward zero.
  EXPECT_EQ(predict_position_q(0, 7, 5, 60), 5);    // 350/60 = 5.83 -> 5.
}

namespace {

EntityRecord delta_test_entity(double pos_y, double vel_y, std::uint8_t state = 0) {
  EntityRecord e;
  e.id = 7;
  e.kind = EntityKind::kRocket;
  e.state = state;
  e.pos_y = pos_y;
  e.vel_y = vel_y;
  return e;
}

}  // namespace

TEST(MessagesTest, DeltaEntityResidualRoundTripsThroughWireAndApply) {
  // 2 ticks of 60 m/s flight plus a small unmodeled (gravity-like) residual.
  const EntityRecord base = delta_test_entity(100.0, 60.0);
  EntityRecord current = delta_test_entity(102.0 - 0.04, 59.7, /*state=*/1);

  DeltaEntity delta = make_delta_entity(base, current, 2, 60);
  ASSERT_EQ(delta.mask & DeltaEntity::kFullRecord, 0) << "small residual must not escape";
  EXPECT_NE(delta.mask & DeltaEntity::kStateChanged, 0);
  EXPECT_EQ(delta.kind(), EntityKind::kRocket);

  Writer w;
  delta.encode(w);
  EXPECT_EQ(w.size(), delta.encoded_size());
  Reader r(w.data());
  const auto decoded = DeltaEntity::decode(r);
  ASSERT_TRUE(decoded.has_value());

  const EntityRecord applied = apply_delta_entity(base, *decoded, 2, 60);
  // The reconstruction matches the QUANTIZED truth — the same fidelity a
  // full snapshot would have delivered.
  EXPECT_DOUBLE_EQ(applied.pos_y, dequantize_position(quantize_position(current.pos_y)));
  EXPECT_DOUBLE_EQ(applied.vel_y, dequantize_velocity(quantize_velocity(current.vel_y)));
  EXPECT_EQ(applied.state, 1);
  EXPECT_EQ(applied.kind, EntityKind::kRocket);
}

TEST(MessagesTest, DeltaEntityEscapesToFullRecordOnBigJump) {
  const EntityRecord base = delta_test_entity(100.0, 60.0);
  const EntityRecord current = delta_test_entity(500.0, 60.0);  // ~398 m off prediction.

  const DeltaEntity delta = make_delta_entity(base, current, 2, 60);
  ASSERT_NE(delta.mask & DeltaEntity::kFullRecord, 0);
  const EntityRecord applied = apply_delta_entity(base, delta, 2, 60);
  EXPECT_DOUBLE_EQ(applied.pos_y, current.pos_y);
}

TEST(MessagesTest, SnapshotDeltaRoundTripAndStrictValidation) {
  SnapshotDelta delta;
  delta.tick = 120;
  delta.base_tick = 118;
  delta.total_entities = 2;
  delta.first_index = 0;
  delta.entities.push_back(make_delta_entity(delta_test_entity(100.0, 60.0),
                                             delta_test_entity(102.0, 60.0), 2, 60));
  DeltaEntity escape;
  escape.id = 9;
  escape.mask = static_cast<std::uint8_t>(
      (static_cast<std::uint8_t>(EntityKind::kTrack) << DeltaEntity::kKindShift) |
      DeltaEntity::kFullRecord);
  escape.full = delta_test_entity(7.0, 1.0);
  delta.entities.push_back(escape);

  Writer w;
  delta.encode(w);
  Reader r(w.data());
  const auto decoded = SnapshotDelta::decode(r);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->base_tick, 118u);
  ASSERT_EQ(decoded->entities.size(), 2u);
  EXPECT_EQ(decoded->entities[1].kind(), EntityKind::kTrack);

  // base_tick >= tick is wire garbage: strict reject.
  delta.base_tick = 120;
  Writer bad;
  delta.encode(bad);
  Reader bad_reader(bad.data());
  EXPECT_FALSE(SnapshotDelta::decode(bad_reader).has_value());

  // Reserved mask bits are rejected (forward-compat stays loud, not silent).
  Writer tampered;
  tampered.u8(0x80);  // Reserved bit set.
  tampered.u16(1);
  Reader tampered_reader(tampered.data());
  EXPECT_FALSE(DeltaEntity::decode(tampered_reader).has_value());
}

TEST(MessagesTest, ServerWelcomeRejectsOutOfRangeWeather) {
  ServerWelcome welcome;
  welcome.rain_intensity = 1.5;  // Outside [0, 1]: strict decoder refuses.
  EXPECT_FALSE(control_round_trip(welcome).has_value());

  welcome.rain_intensity = 0.5;
  welcome.gust_sigma_mps = -1.0;  // Negative σ is meaningless.
  EXPECT_FALSE(control_round_trip(welcome).has_value());
}

TEST(MessagesTest, ServerRejectRoundTrip) {
  const auto decoded = control_round_trip(ServerReject{RejectReason::kRoleTaken});
  ASSERT_TRUE(decoded.has_value());
  const auto* m = std::get_if<ServerReject>(&*decoded);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->reason, RejectReason::kRoleTaken);
}

TEST(MessagesTest, FireRequestRoundTripsDoublesBitExactly) {
  FireRequest fire;
  fire.azimuth_rad = 0.7853981633974483;
  fire.elevation_rad = 0.5235987755982988;
  fire.salvo_count = 12;
  fire.dispersion_mrad = 7.25;
  fire.launch_interval_s = 0.05;

  const auto decoded = control_round_trip(fire);
  ASSERT_TRUE(decoded.has_value());
  const auto* m = std::get_if<FireRequest>(&*decoded);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->azimuth_rad, 0.7853981633974483);
  EXPECT_EQ(m->elevation_rad, 0.5235987755982988);
  EXPECT_EQ(m->salvo_count, 12);
  EXPECT_EQ(m->dispersion_mrad, 7.25);
  EXPECT_EQ(m->launch_interval_s, 0.05);
}

TEST(MessagesTest, UdpHelloAndEmptyMessagesRoundTrip) {
  const auto hello = data_round_trip(UdpHello{123456789});
  ASSERT_TRUE(hello.has_value());
  EXPECT_EQ(std::get<UdpHello>(*hello).token, 123456789u);

  EXPECT_TRUE(data_round_trip(UdpHelloAck{}).has_value());
  EXPECT_TRUE(data_round_trip(Keepalive{}).has_value());
}

TEST(MessagesTest, QuantizationErrorIsBounded) {
  // Positions: half an LSB (0.5 cm); velocities: half of 0.1 m/s.
  const double positions[] = {0.0, 2.0, -38000.4567, 9999.991, 83000.0, -0.005};
  for (const double p : positions) {
    const double back = dequantize_position(quantize_position(p));
    EXPECT_NEAR(back, p, kPositionLsbM / 2 + 1e-9) << "position " << p;
  }
  const double velocities[] = {0.0, 250.0, -290.13, 871.49, -3276.0};
  for (const double v : velocities) {
    const double back = dequantize_velocity(quantize_velocity(v));
    EXPECT_NEAR(back, v, kVelocityLsbMps / 2 + 1e-9) << "velocity " << v;
  }
}

TEST(MessagesTest, QuantizationClampsOutOfRangeAndNonFinite) {
  EXPECT_EQ(quantize_position(1e9), kPositionQMax);
  EXPECT_EQ(quantize_position(-1e9), -kPositionQMax - 1);
  EXPECT_EQ(quantize_position(std::nan("")), 0);
  EXPECT_EQ(quantize_velocity(1e9), INT16_MAX);
  EXPECT_EQ(quantize_velocity(-1e9), INT16_MIN);
  EXPECT_EQ(quantize_velocity(std::nan("")), 0);
}

TEST(MessagesTest, EntityRecordIsExactly20BytesOnTheWire) {
  EntityRecord e;
  e.id = 7;
  e.kind = EntityKind::kRocket;
  e.state = 1;
  e.pos_x = 1234.56;
  e.pos_y = -8000.0;
  e.pos_z = 305.07;
  e.vel_x = -250.0;
  e.vel_y = 12.3;
  e.vel_z = -88.8;

  Writer w;
  e.encode(w);
  EXPECT_EQ(w.size(), kEntityRecordBytes);

  Reader r(w.data());
  const auto decoded = EntityRecord::decode(r);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(r.finished());
  EXPECT_EQ(decoded->id, 7);
  EXPECT_EQ(decoded->kind, EntityKind::kRocket);
  EXPECT_EQ(decoded->state, 1);
  EXPECT_NEAR(decoded->pos_x, 1234.56, kPositionLsbM / 2 + 1e-9);
  EXPECT_NEAR(decoded->pos_z, 305.07, kPositionLsbM / 2 + 1e-9);
  EXPECT_NEAR(decoded->vel_x, -250.0, kVelocityLsbMps / 2 + 1e-9);
  EXPECT_NEAR(decoded->vel_z, -88.8, kVelocityLsbMps / 2 + 1e-9);
}

TEST(MessagesTest, SnapshotBatchRoundTrip) {
  Snapshot snap;
  snap.tick = 1200;
  snap.phase = EngagementPhase::kRunning;
  snap.total_entities = 100;
  snap.first_index = 58;
  for (int i = 0; i < 42; ++i) {
    EntityRecord e;
    e.id = static_cast<std::uint16_t>(i);
    e.kind = EntityKind::kRocket;
    e.pos_x = 100.0 * i;
    snap.entities.push_back(e);
  }

  const auto decoded = data_round_trip(snap);
  ASSERT_TRUE(decoded.has_value());
  const auto* m = std::get_if<Snapshot>(&*decoded);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->tick, 1200u);
  EXPECT_EQ(m->total_entities, 100);
  EXPECT_EQ(m->first_index, 58);
  ASSERT_EQ(m->entities.size(), 42u);
  EXPECT_NEAR(m->entities[41].pos_x, 4100.0, kPositionLsbM / 2 + 1e-9);
}

TEST(MessagesTest, SnapshotRejectsInconsistentBatchSlice) {
  Snapshot snap;
  snap.tick = 1;
  snap.total_entities = 10;
  snap.first_index = 8;
  snap.entities.resize(3);  // 8 + 3 > 10.
  EXPECT_FALSE(data_round_trip(snap).has_value());
}

TEST(MessagesTest, EngagementEventRoundTrip) {
  EngagementEvent ev;
  ev.tick = 777;
  ev.kind = EventKind::kRocketResolved;
  ev.subject_id = 5;
  ev.miss_distance_m = 5.3F;
  ev.detonated = true;
  ev.killed = true;

  const auto decoded = data_round_trip(ev);
  ASSERT_TRUE(decoded.has_value());
  const auto* m = std::get_if<EngagementEvent>(&*decoded);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->tick, 777u);
  EXPECT_EQ(m->kind, EventKind::kRocketResolved);
  EXPECT_EQ(m->subject_id, 5);
  EXPECT_EQ(m->miss_distance_m, 5.3F);
  EXPECT_TRUE(m->detonated);
  EXPECT_TRUE(m->killed);
}

TEST(MessagesTest, TrackEntityRoundTripsWithSigmaFlags) {
  EntityRecord track;
  track.id = 3;
  track.kind = EntityKind::kTrack;
  track.state = 2;  // Coasting.
  track.flags = quantize_track_sigma(45.0);
  track.pos_x = 5200.0;
  track.vel_y = -240.0;

  Writer w;
  track.encode(w);
  EXPECT_EQ(w.size(), kEntityRecordBytes);
  Reader r(w.data());
  const auto decoded = EntityRecord::decode(r);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->kind, EntityKind::kTrack);
  EXPECT_EQ(decoded->state, 2);
  // Log quantization: ~4.7% per step round-trip accuracy.
  EXPECT_NEAR(dequantize_track_sigma(decoded->flags), 45.0, 45.0 * 0.05);
}

TEST(MessagesTest, TrackSigmaQuantizationCoversTheLogRange) {
  EXPECT_EQ(quantize_track_sigma(0.0), 0);
  EXPECT_EQ(quantize_track_sigma(-1.0), 0);
  EXPECT_EQ(quantize_track_sigma(0.1), 0);
  EXPECT_EQ(quantize_track_sigma(1e9), 255);
  for (const double sigma : {0.5, 5.0, 50.0, 500.0, 5000.0}) {
    const double back = dequantize_track_sigma(quantize_track_sigma(sigma));
    EXPECT_NEAR(back, sigma, sigma * 0.05) << "sigma " << sigma;
  }
}

TEST(MessagesTest, TrackLifecycleEventsRoundTrip) {
  for (const EventKind kind : {EventKind::kTrackConfirmed, EventKind::kTrackLost}) {
    EngagementEvent ev;
    ev.tick = 360;
    ev.kind = kind;
    ev.subject_id = 7;  // Track id rides the renamed subject field.
    const auto decoded = data_round_trip(ev);
    ASSERT_TRUE(decoded.has_value());
    const auto* m = std::get_if<EngagementEvent>(&*decoded);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->kind, kind);
    EXPECT_EQ(m->subject_id, 7);
  }
}

TEST(MessagesTest, FireRequestCarriesTrackDesignation) {
  FireRequest fire;
  fire.track_id = 12;
  fire.azimuth_rad = 0.01;  // Operator offset in track mode.
  const auto decoded = control_round_trip(fire);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(std::get<FireRequest>(*decoded).track_id, 12);
}

TEST(MessagesTest, FireSolutionRoundTrips) {
  FireSolution solution;
  solution.tick = 720;
  solution.track_id = 4;
  solution.valid = true;
  solution.pip_x = 3120.55;
  solution.pip_y = -880.21;
  solution.pip_z = 410.07;
  solution.time_of_flight_s = 9.25F;
  solution.dispersion_radius_m = 38.5F;

  const auto decoded = data_round_trip(solution);
  ASSERT_TRUE(decoded.has_value());
  const auto* m = std::get_if<FireSolution>(&*decoded);
  ASSERT_NE(m, nullptr);
  EXPECT_TRUE(m->valid);
  EXPECT_EQ(m->track_id, 4);
  EXPECT_NEAR(m->pip_x, 3120.55, kPositionLsbM / 2 + 1e-9);
  EXPECT_NEAR(m->pip_z, 410.07, kPositionLsbM / 2 + 1e-9);
  EXPECT_EQ(m->time_of_flight_s, 9.25F);
}

TEST(MessagesTest, V2GuardsAcceptNewEnumsAndRejectTheNextOnes) {
  // The v1->v2 trap: forgetting the guard would make the server reject its
  // OWN snapshots. Pin both sides of each boundary.
  Writer w;
  w.u16(1);
  w.u8(static_cast<std::uint8_t>(EntityKind::kTrack));
  w.u8(0);
  w.u8(0);
  for (int i = 0; i < 3; ++i) w.i24(0);
  for (int i = 0; i < 3; ++i) w.i16(0);
  Reader r(w.data());
  EXPECT_TRUE(EntityRecord::decode(r).has_value());

  Writer w2;
  w2.u16(1);
  w2.u8(3);  // One past kTrack.
  w2.u8(0);
  w2.u8(0);
  for (int i = 0; i < 3; ++i) w2.i24(0);
  for (int i = 0; i < 3; ++i) w2.i16(0);
  Reader r2(w2.data());
  EXPECT_FALSE(EntityRecord::decode(r2).has_value());

  // Game-mode event kinds (kTargetHitShip=6, kRoundStart=7) are valid; the
  // first value past the catalogue (8) is rejected.
  for (const std::uint8_t kind : {std::uint8_t{6}, std::uint8_t{7}}) {
    Writer wok;
    wok.u32(1);
    wok.u8(kind);
    wok.u16(0);
    wok.f32(0.0F);
    wok.u8(0);
    wok.u8(0);
    EXPECT_TRUE(decode_data_message(MsgType::kEngagementEvent, wok.data()).has_value());
  }

  Writer w3;
  w3.u32(1);
  w3.u8(8);  // One past kRoundStart.
  w3.u16(0);
  w3.f32(0.0F);
  w3.u8(0);
  w3.u8(0);
  EXPECT_FALSE(decode_data_message(MsgType::kEngagementEvent, w3.data()).has_value());
}

TEST(MessagesTest, DecodeRejectsUnknownTypeTruncationAndTrailingBytes) {
  // Unknown control type.
  const std::uint8_t unknown[] = {0xEE, 0x01, 0x02};
  EXPECT_FALSE(decode_control_frame(unknown).has_value());

  // Empty frame.
  EXPECT_FALSE(decode_control_frame({}).has_value());

  // Truncated payload.
  auto frame = encode_control_frame(ClientHello{});
  frame.pop_back();
  EXPECT_FALSE(decode_control_frame(frame).has_value());

  // Trailing garbage.
  auto frame2 = encode_control_frame(ServerReject{RejectReason::kServerFull});
  frame2.push_back(0x00);
  EXPECT_FALSE(decode_control_frame(frame2).has_value());

  // Wrong data-message type for the payload.
  EXPECT_FALSE(decode_data_message(MsgType::kSnapshot, encode_payload(UdpHello{1})).has_value());
}

TEST(MessagesTest, DecodeRejectsOutOfRangeEnums) {
  // Role 4 does not exist.
  Writer w;
  w.u8(static_cast<std::uint8_t>(MsgType::kClientHello));
  w.u16(kProtocolVersion);
  w.u8(4);
  w.u64(0);
  EXPECT_FALSE(decode_control_frame(w.data()).has_value());

  // EventKind 9 does not exist.
  Writer w2;
  w2.u32(1);
  w2.u8(9);
  w2.u16(0);
  w2.f32(0.0F);
  w2.u8(0);
  w2.u8(0);
  EXPECT_FALSE(decode_data_message(MsgType::kEngagementEvent, w2.data()).has_value());
}

}  // namespace
}  // namespace seashield::protocol
