#include "net/frame_parser.h"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace seashield::net {
namespace {

std::vector<std::uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

std::vector<std::uint8_t> framed(const std::string& payload) {
  std::vector<std::uint8_t> out;
  const auto p = bytes(payload);
  FrameParser::encode(out, p);
  return out;
}

class Collector {
 public:
  FrameParser::FrameHandler handler() {
    return [this](std::span<const std::uint8_t> frame) {
      frames_.emplace_back(frame.begin(), frame.end());
    };
  }
  std::vector<std::vector<std::uint8_t>> frames_;
};

TEST(FrameParserTest, EncodeWritesLittleEndianHeader) {
  std::vector<std::uint8_t> out;
  const std::vector<std::uint8_t> payload(0x0102, 0xAB);
  FrameParser::encode(out, payload);
  ASSERT_EQ(out.size(), FrameParser::kHeaderSize + payload.size());
  EXPECT_EQ(out[0], 0x02);  // Low byte first.
  EXPECT_EQ(out[1], 0x01);
}

TEST(FrameParserTest, SingleExactFrame) {
  FrameParser parser;
  Collector collector;
  EXPECT_TRUE(parser.feed(framed("hello"), collector.handler()));
  ASSERT_EQ(collector.frames_.size(), 1u);
  EXPECT_EQ(collector.frames_[0], bytes("hello"));
  EXPECT_EQ(parser.buffered_bytes(), 0u);
}

TEST(FrameParserTest, MultipleFramesInOneFeed) {
  FrameParser parser;
  Collector collector;
  auto wire = framed("one");
  const auto second = framed("two");
  const auto third = framed("three");
  wire.insert(wire.end(), second.begin(), second.end());
  wire.insert(wire.end(), third.begin(), third.end());

  EXPECT_TRUE(parser.feed(wire, collector.handler()));
  ASSERT_EQ(collector.frames_.size(), 3u);
  EXPECT_EQ(collector.frames_[2], bytes("three"));
}

TEST(FrameParserTest, OneAndAHalfFramesThenCompletion) {
  FrameParser parser;
  Collector collector;
  auto wire = framed("complete");
  const auto next = framed("pending");
  // Append only half of the second frame.
  const std::size_t half = next.size() / 2;
  wire.insert(wire.end(), next.begin(), next.begin() + static_cast<std::ptrdiff_t>(half));

  EXPECT_TRUE(parser.feed(wire, collector.handler()));
  ASSERT_EQ(collector.frames_.size(), 1u);
  EXPECT_GT(parser.buffered_bytes(), 0u);

  EXPECT_TRUE(parser.feed({next.data() + half, next.size() - half}, collector.handler()));
  ASSERT_EQ(collector.frames_.size(), 2u);
  EXPECT_EQ(collector.frames_[1], bytes("pending"));
  EXPECT_EQ(parser.buffered_bytes(), 0u);
}

TEST(FrameParserTest, ByteByByteFeed) {
  FrameParser parser;
  Collector collector;
  const auto wire = framed("drip");
  for (const std::uint8_t byte : wire) {
    EXPECT_TRUE(parser.feed({&byte, 1}, collector.handler()));
  }
  ASSERT_EQ(collector.frames_.size(), 1u);
  EXPECT_EQ(collector.frames_[0], bytes("drip"));
}

TEST(FrameParserTest, ZeroLengthFrameIsViolation) {
  FrameParser parser;
  Collector collector;
  const std::vector<std::uint8_t> wire = {0x00, 0x00};
  EXPECT_FALSE(parser.feed(wire, collector.handler()));
  EXPECT_TRUE(collector.frames_.empty());
}

TEST(FrameParserTest, OversizedFrameIsViolation) {
  FrameParser parser;
  Collector collector;
  const std::size_t length = FrameParser::kMaxPayloadSize + 1;
  const std::vector<std::uint8_t> wire = {static_cast<std::uint8_t>(length & 0xFF),
                                          static_cast<std::uint8_t>(length >> 8)};
  EXPECT_FALSE(parser.feed(wire, collector.handler()));
}

TEST(FrameParserTest, MaxSizeFrameIsAccepted) {
  FrameParser parser;
  Collector collector;
  std::vector<std::uint8_t> wire;
  const std::vector<std::uint8_t> payload(FrameParser::kMaxPayloadSize, 0x42);
  FrameParser::encode(wire, payload);
  EXPECT_TRUE(parser.feed(wire, collector.handler()));
  ASSERT_EQ(collector.frames_.size(), 1u);
  EXPECT_EQ(collector.frames_[0].size(), FrameParser::kMaxPayloadSize);
}

}  // namespace
}  // namespace seashield::net
