#include <gtest/gtest.h>

#include "client/core/coords.h"
#include "client/core/interp_buffer.h"

// The client presentation core (charter §7): frame mapping, batched-snapshot
// reassembly under loss/reordering, and render-time interpolation rules.
namespace seashield::client {
namespace {

protocol::EntityRecord entity(protocol::EntityKind kind, std::uint16_t id, double pos_y,
                              double vel_y = 0.0) {
  protocol::EntityRecord record;
  record.kind = kind;
  record.id = id;
  record.pos_y = pos_y;
  record.vel_y = vel_y;
  return record;
}

protocol::Snapshot batch(std::uint32_t tick, std::uint16_t total, std::uint16_t first,
                         std::vector<protocol::EntityRecord> entities) {
  protocol::Snapshot snap;
  snap.tick = tick;
  snap.total_entities = total;
  snap.first_index = first;
  snap.entities = std::move(entities);
  return snap;
}

CompletedSnapshot completed(std::uint32_t tick, std::vector<protocol::EntityRecord> entities) {
  CompletedSnapshot snap;
  snap.tick = tick;
  snap.entities = std::move(entities);
  return snap;
}

TEST(ClientCoordsTest, MapsEnuAxesOntoUe) {
  // East -> UE right (+Y), north -> UE forward (+X), up -> up; meters -> cm.
  const UeVector east = to_ue_cm(1.0, 0.0, 0.0);
  EXPECT_DOUBLE_EQ(east.x, 0.0);
  EXPECT_DOUBLE_EQ(east.y, 100.0);
  const UeVector north = to_ue_cm(0.0, 1.0, 0.0);
  EXPECT_DOUBLE_EQ(north.x, 100.0);
  EXPECT_DOUBLE_EQ(north.y, 0.0);
  const UeVector up = to_ue_cm(0.0, 0.0, 2.5);
  EXPECT_DOUBLE_EQ(up.z, 250.0);

  const EnuVector back = to_enu_m(to_ue_cm(3.0, -4.0, 5.0));
  EXPECT_DOUBLE_EQ(back.east_m, 3.0);
  EXPECT_DOUBLE_EQ(back.north_m, -4.0);
  EXPECT_DOUBLE_EQ(back.up_m, 5.0);

  // Azimuth 0 = north = UE yaw 0; azimuth 90° (east) = yaw 90°.
  EXPECT_DOUBLE_EQ(azimuth_rad_to_ue_yaw_deg(0.0), 0.0);
  EXPECT_NEAR(azimuth_rad_to_ue_yaw_deg(std::numbers::pi / 2.0), 90.0, 1e-12);
  EXPECT_NEAR(ue_yaw_deg_to_azimuth_rad(180.0), std::numbers::pi, 1e-12);
}

TEST(SnapshotAssemblerTest, CompletesMultiBatchOutOfOrder) {
  SnapshotAssembler assembler;
  const auto kind = protocol::EntityKind::kRocket;
  // 5 entities split [3..5) then [0..3); the second batch completes the tick.
  EXPECT_FALSE(
      assembler.push(batch(10, 5, 3, {entity(kind, 3, 3.0), entity(kind, 4, 4.0)})));
  const auto done = assembler.push(
      batch(10, 5, 0, {entity(kind, 0, 0.0), entity(kind, 1, 1.0), entity(kind, 2, 2.0)}));
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(done->tick, 10u);
  ASSERT_EQ(done->entities.size(), 5u);
  for (std::uint16_t i = 0; i < 5; ++i) {
    EXPECT_EQ(done->entities[i].id, i);  // Reassembled in index order.
  }
}

TEST(SnapshotAssemblerTest, StaleTickIsDroppedAfterANewerCompletion) {
  SnapshotAssembler assembler;
  const auto kind = protocol::EntityKind::kTarget;
  ASSERT_TRUE(assembler.push(batch(20, 1, 0, {entity(kind, 0, 0.0)})).has_value());
  // A frame older than the completed one must never complete afterwards.
  EXPECT_FALSE(assembler.push(batch(18, 1, 0, {entity(kind, 0, 9.0)})).has_value());
  EXPECT_FALSE(assembler.push(batch(18, 1, 0, {entity(kind, 0, 9.0)})).has_value());
}

TEST(SnapshotAssemblerTest, EmptySnapshotCompletesImmediately) {
  SnapshotAssembler assembler;
  const auto done = assembler.push(batch(5, 0, 0, {}));
  ASSERT_TRUE(done.has_value());
  EXPECT_TRUE(done->entities.empty());
}

TEST(SnapshotAssemblerTest, DuplicateBatchDoesNotDoubleCount) {
  SnapshotAssembler assembler;
  const auto kind = protocol::EntityKind::kRocket;
  EXPECT_FALSE(assembler.push(batch(7, 2, 0, {entity(kind, 0, 0.0)})).has_value());
  EXPECT_FALSE(assembler.push(batch(7, 2, 0, {entity(kind, 0, 0.0)})).has_value());
  EXPECT_TRUE(assembler.push(batch(7, 2, 1, {entity(kind, 1, 1.0)})).has_value());
}

TEST(SnapshotAssemblerTest, AppliesDeltaAgainstAckedBaseline) {
  SnapshotAssembler assembler;
  const auto rocket = protocol::EntityKind::kRocket;
  const protocol::EntityRecord a = entity(rocket, 1, 100.0, 60.0);
  const protocol::EntityRecord b = entity(rocket, 2, 50.0, 0.0);
  ASSERT_TRUE(assembler.push(batch(10, 2, 0, {a, b})).has_value());
  ASSERT_EQ(assembler.latest_completed_tick().value(), 10u);

  // Tick 12 vs base 10: rocket 1 flew on (~2 m), rocket 2 is gone (implicit
  // removal), and a new track arrives as a full-record escape.
  protocol::SnapshotDelta delta;
  delta.tick = 12;
  delta.base_tick = 10;
  delta.total_entities = 2;
  delta.first_index = 0;
  delta.entities.push_back(
      protocol::make_delta_entity(a, entity(rocket, 1, 102.0, 60.0), 2, 60));
  protocol::DeltaEntity escape;
  escape.id = 9;
  escape.mask = static_cast<std::uint8_t>(
      (static_cast<std::uint8_t>(protocol::EntityKind::kTrack)
       << protocol::DeltaEntity::kKindShift) |
      protocol::DeltaEntity::kFullRecord);
  escape.full = entity(protocol::EntityKind::kTrack, 9, 7.0);
  delta.entities.push_back(escape);

  const auto done = assembler.push_delta(delta);
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(done->tick, 12u);
  ASSERT_EQ(done->entities.size(), 2u);
  EXPECT_NEAR(done->entities[0].pos_y, 102.0, 0.02);  // Quantization tolerance.
  EXPECT_EQ(done->entities[1].kind, protocol::EntityKind::kTrack);
  EXPECT_EQ(assembler.latest_completed_tick().value(), 12u);  // What gets acked next.
}

TEST(SnapshotAssemblerTest, DeltaWithUnknownBaselineIsDroppedAndStreamHeals) {
  SnapshotAssembler assembler;
  const auto rocket = protocol::EntityKind::kRocket;
  protocol::SnapshotDelta delta;
  delta.tick = 12;
  delta.base_tick = 10;  // Never completed here.
  delta.total_entities = 1;
  delta.first_index = 0;
  delta.entities.push_back(protocol::make_delta_entity(entity(rocket, 1, 100.0),
                                                       entity(rocket, 1, 100.5), 2, 60));
  EXPECT_FALSE(assembler.push_delta(delta).has_value());
  // The server's full-snapshot fallback then heals the stream.
  EXPECT_TRUE(assembler.push(batch(14, 1, 0, {entity(rocket, 1, 101.0)})).has_value());
}

TEST(SnapshotAssemblerTest, DeltaBatchesReassembleOutOfOrder) {
  SnapshotAssembler assembler;
  const auto rocket = protocol::EntityKind::kRocket;
  std::vector<protocol::EntityRecord> base_entities;
  for (std::uint16_t i = 0; i < 3; ++i) {
    base_entities.push_back(entity(rocket, i, 10.0 * i, 60.0));
  }
  ASSERT_TRUE(assembler.push(batch(10, 3, 0, base_entities)).has_value());

  const auto make_batch = [&](std::uint16_t first, std::uint16_t count) {
    protocol::SnapshotDelta delta;
    delta.tick = 12;
    delta.base_tick = 10;
    delta.total_entities = 3;
    delta.first_index = first;
    for (std::uint16_t i = first; i < first + count; ++i) {
      delta.entities.push_back(protocol::make_delta_entity(
          base_entities[i], entity(rocket, i, 10.0 * i + 2.0, 60.0), 2, 60));
    }
    return delta;
  };
  EXPECT_FALSE(assembler.push_delta(make_batch(2, 1)).has_value());
  const auto done = assembler.push_delta(make_batch(0, 2));
  ASSERT_TRUE(done.has_value());
  ASSERT_EQ(done->entities.size(), 3u);
  EXPECT_NEAR(done->entities[2].pos_y, 22.0, 0.02);
}

TEST(InterpolationBufferTest, LerpsBetweenBracketingSnapshots) {
  InterpolationBuffer buffer;
  const auto kind = protocol::EntityKind::kRocket;
  buffer.push(completed(0, {entity(kind, 1, 0.0, 60.0)}));
  buffer.push(completed(2, {entity(kind, 1, 2.0, 60.0)}));

  const auto mid = buffer.sample(1.0);
  ASSERT_EQ(mid.size(), 1u);
  EXPECT_DOUBLE_EQ(mid[0].pos_y, 1.0);
  EXPECT_FALSE(mid[0].extrapolated);

  const auto exact = buffer.sample(2.0);
  ASSERT_EQ(exact.size(), 1u);
  EXPECT_DOUBLE_EQ(exact[0].pos_y, 2.0);
}

TEST(InterpolationBufferTest, TracksSnapInsteadOfLerping) {
  InterpolationBuffer buffer;
  const auto kind = protocol::EntityKind::kTrack;
  buffer.push(completed(0, {entity(kind, 1, 0.0)}));
  buffer.push(completed(2, {entity(kind, 1, 2.0)}));

  // A sensor estimate must not be smoothed into fake continuity (§5.5).
  const auto sampled = buffer.sample(1.5);
  ASSERT_EQ(sampled.size(), 1u);
  EXPECT_DOUBLE_EQ(sampled[0].pos_y, 0.0);
  EXPECT_DOUBLE_EQ(buffer.sample(2.0)[0].pos_y, 2.0);
}

TEST(InterpolationBufferTest, ExtrapolationIsCapped) {
  InterpolationBuffer buffer(60.0, 0.25);
  const auto kind = protocol::EntityKind::kRocket;
  buffer.push(completed(0, {entity(kind, 1, 0.0, 60.0)}));  // 60 m/s along y.

  // 30 ticks (0.5 s) past the last snapshot: dead-reckoning stops at 0.25 s.
  const auto sampled = buffer.sample(30.0);
  ASSERT_EQ(sampled.size(), 1u);
  EXPECT_DOUBLE_EQ(sampled[0].pos_y, 60.0 * 0.25);
  EXPECT_TRUE(sampled[0].extrapolated);
}

TEST(InterpolationBufferTest, RenderTickLagsTheNewestSnapshot) {
  InterpolationBuffer buffer;
  EXPECT_FALSE(buffer.render_tick(6.0).has_value());
  buffer.push(completed(100, {}));
  buffer.push(completed(102, {}));
  EXPECT_DOUBLE_EQ(buffer.render_tick(6.0).value(), 96.0);
}

}  // namespace
}  // namespace seashield::client
