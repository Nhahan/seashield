#include "protocol/wire.h"

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

namespace seashield::protocol {
namespace {

TEST(WireTest, PrimitivesRoundTrip) {
  Writer w;
  w.u8(0xAB);
  w.u16(0xBEEF);
  w.u32(0xDEADBEEFu);
  w.u64(0x0123456789ABCDEFull);
  w.i16(-12345);
  w.i32(-123456789);
  w.f32(3.5F);
  w.f64(-2.718281828459045);

  Reader r(w.data());
  EXPECT_EQ(r.u8(), 0xAB);
  EXPECT_EQ(r.u16(), 0xBEEF);
  EXPECT_EQ(r.u32(), 0xDEADBEEFu);
  EXPECT_EQ(r.u64(), 0x0123456789ABCDEFull);
  EXPECT_EQ(r.i16(), -12345);
  EXPECT_EQ(r.i32(), -123456789);
  EXPECT_EQ(r.f32(), 3.5F);
  EXPECT_EQ(r.f64(), -2.718281828459045);
  EXPECT_TRUE(r.finished());
}

TEST(WireTest, LittleEndianLayoutIsExplicit) {
  Writer w;
  w.u16(0x1234);
  w.u32(0x89ABCDEFu);
  const auto& b = w.data();
  ASSERT_EQ(b.size(), 6u);
  EXPECT_EQ(b[0], 0x34);
  EXPECT_EQ(b[1], 0x12);
  EXPECT_EQ(b[2], 0xEF);
  EXPECT_EQ(b[3], 0xCD);
  EXPECT_EQ(b[4], 0xAB);
  EXPECT_EQ(b[5], 0x89);
}

TEST(WireTest, I24RoundTripsAcrossFullRange) {
  const std::int32_t values[] = {0, 1, -1, 0x7FFFFF, -0x800000, 12345, -654321};
  for (const std::int32_t v : values) {
    Writer w;
    w.i24(v);
    ASSERT_EQ(w.size(), 3u);
    Reader r(w.data());
    EXPECT_EQ(r.i24(), v) << "value " << v;
    EXPECT_TRUE(r.finished());
  }
}

TEST(WireTest, F64RoundTripsBitExactly) {
  const double values[] = {0.0, -0.0, 1e-300, -1e300, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()};
  for (const double v : values) {
    Writer w;
    w.f64(v);
    Reader r(w.data());
    EXPECT_EQ(std::bit_cast<std::uint64_t>(r.f64()), std::bit_cast<std::uint64_t>(v));
  }
}

TEST(WireTest, StringRoundTrip) {
  Writer w;
  w.str16("");
  w.str16("light rain, wind 8 m/s from 240°");
  Reader r(w.data());
  EXPECT_EQ(r.str16(), "");
  EXPECT_EQ(r.str16(), "light rain, wind 8 m/s from 240°");
  EXPECT_TRUE(r.finished());
}

TEST(WireTest, ReaderShortBufferFailsSticky) {
  Writer w;
  w.u16(7);
  Reader r(w.data());
  EXPECT_EQ(r.u32(), 0u);  // 4 bytes requested, 2 available.
  EXPECT_FALSE(r.ok());
  // Sticky: even reads that would fit now fail and return zero.
  EXPECT_EQ(r.u8(), 0u);
  EXPECT_FALSE(r.finished());
}

TEST(WireTest, ReaderStringLengthBeyondBufferFails) {
  Writer w;
  w.u16(100);  // Claims 100 bytes; none follow.
  Reader r(w.data());
  EXPECT_EQ(r.str16(), "");
  EXPECT_FALSE(r.ok());
}

TEST(WireTest, FinishedRequiresExactConsumption) {
  Writer w;
  w.u32(42);
  Reader r(w.data());
  EXPECT_EQ(r.u16(), 42u);
  EXPECT_TRUE(r.ok());
  EXPECT_FALSE(r.finished());  // 2 trailing bytes -> not finished.
  EXPECT_EQ(r.remaining(), 2u);
}

TEST(WireTest, BytesViewsWithoutCopying) {
  Writer w;
  const std::uint8_t payload[] = {1, 2, 3, 4};
  w.bytes(payload);
  Reader r(w.data());
  const auto view = r.bytes(4);
  ASSERT_EQ(view.size(), 4u);
  EXPECT_EQ(view[2], 3);
  EXPECT_TRUE(r.finished());
}

TEST(WireTest, EmptyReaderReportsFinished) {
  Reader r({});
  EXPECT_TRUE(r.ok());
  EXPECT_TRUE(r.finished());
  EXPECT_EQ(r.u8(), 0u);
  EXPECT_FALSE(r.ok());
}

}  // namespace
}  // namespace seashield::protocol
