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
  ev.rocket_id = 5;
  ev.miss_distance_m = 5.3F;
  ev.detonated = true;
  ev.killed = true;

  const auto decoded = data_round_trip(ev);
  ASSERT_TRUE(decoded.has_value());
  const auto* m = std::get_if<EngagementEvent>(&*decoded);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->tick, 777u);
  EXPECT_EQ(m->kind, EventKind::kRocketResolved);
  EXPECT_EQ(m->rocket_id, 5);
  EXPECT_EQ(m->miss_distance_m, 5.3F);
  EXPECT_TRUE(m->detonated);
  EXPECT_TRUE(m->killed);
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
