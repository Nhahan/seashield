#include "net/send_queue.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace seashield::net {
namespace {

std::vector<std::uint8_t> chunk(std::size_t n, std::uint8_t value = 0xAA) {
  return std::vector<std::uint8_t>(n, value);
}

TEST(SendQueueTest, PushAccumulatesWithinCap) {
  SendQueue queue(100);
  EXPECT_TRUE(queue.push(chunk(40)));
  EXPECT_TRUE(queue.push(chunk(60)));
  EXPECT_EQ(queue.size_bytes(), 100u);
  EXPECT_FALSE(queue.overflowed());
}

TEST(SendQueueTest, OverflowRejectsAndFlags) {
  SendQueue queue(100);
  EXPECT_TRUE(queue.push(chunk(80)));
  EXPECT_FALSE(queue.push(chunk(21)));  // 101 > 100
  EXPECT_TRUE(queue.overflowed());
  EXPECT_EQ(queue.size_bytes(), 80u);  // Rejected chunk was not queued.
}

TEST(SendQueueTest, FlushDrainsEverything) {
  SendQueue queue(1024);
  queue.push(chunk(10, 1));
  queue.push(chunk(20, 2));

  std::vector<std::uint8_t> wire;
  const auto result = queue.flush([&](const std::uint8_t* data, std::size_t size) {
    wire.insert(wire.end(), data, data + size);
    return static_cast<long>(size);
  });

  EXPECT_EQ(result, SendQueue::FlushResult::kDrained);
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.size_bytes(), 0u);
  EXPECT_EQ(wire.size(), 30u);
  EXPECT_EQ(wire[0], 1);
  EXPECT_EQ(wire[29], 2);
}

TEST(SendQueueTest, PartialWriteResumesAtOffset) {
  SendQueue queue(1024);
  std::vector<std::uint8_t> data;
  for (std::uint8_t i = 0; i < 10; ++i) {
    data.push_back(i);
  }
  queue.push(std::move(data));

  std::vector<std::uint8_t> wire;
  // First flush: the kernel "accepts" only 3 bytes, then would-block.
  auto result = queue.flush([&](const std::uint8_t* d, std::size_t) {
    wire.insert(wire.end(), d, d + 3);
    return 3L;
  });
  EXPECT_EQ(result, SendQueue::FlushResult::kWouldBlock);
  EXPECT_EQ(queue.size_bytes(), 7u);

  // Second flush resumes exactly where the partial write stopped.
  result = queue.flush([&](const std::uint8_t* d, std::size_t size) {
    wire.insert(wire.end(), d, d + size);
    return static_cast<long>(size);
  });
  EXPECT_EQ(result, SendQueue::FlushResult::kDrained);
  ASSERT_EQ(wire.size(), 10u);
  for (std::uint8_t i = 0; i < 10; ++i) {
    EXPECT_EQ(wire[i], i);
  }
}

TEST(SendQueueTest, WouldBlockKeepsData) {
  SendQueue queue(1024);
  queue.push(chunk(50));
  const auto result = queue.flush([](const std::uint8_t*, std::size_t) { return 0L; });
  EXPECT_EQ(result, SendQueue::FlushResult::kWouldBlock);
  EXPECT_EQ(queue.size_bytes(), 50u);
}

TEST(SendQueueTest, WriterErrorPropagates) {
  SendQueue queue(1024);
  queue.push(chunk(50));
  const auto result = queue.flush([](const std::uint8_t*, std::size_t) { return -1L; });
  EXPECT_EQ(result, SendQueue::FlushResult::kError);
}

TEST(SendQueueTest, EmptyPushIsNoop) {
  SendQueue queue(10);
  EXPECT_TRUE(queue.push({}));
  EXPECT_TRUE(queue.empty());
}

}  // namespace
}  // namespace seashield::net
