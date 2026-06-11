#include "core/spsc_queue.h"

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace seashield {
namespace {

TEST(SpscQueueTest, FifoOrderSingleThread) {
  SpscQueue<int> queue(8);
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(queue.push(int(i)));
  }
  EXPECT_EQ(queue.size(), 5u);
  int out = -1;
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(queue.pop(out));
    EXPECT_EQ(out, i);
  }
  EXPECT_FALSE(queue.pop(out));
  EXPECT_TRUE(queue.empty());
}

TEST(SpscQueueTest, FullRingRejectsPushWithoutBlocking) {
  SpscQueue<int> queue(4);
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(queue.push(int(i)));
  }
  EXPECT_FALSE(queue.push(99)) << "push into a full ring must fail, not block";
  int out = -1;
  ASSERT_TRUE(queue.pop(out));
  EXPECT_EQ(out, 0);
  EXPECT_TRUE(queue.push(4)) << "slot freed by pop must be reusable";
  EXPECT_EQ(queue.size(), 4u);
}

TEST(SpscQueueTest, IndicesWrapAroundTheRingMask) {
  SpscQueue<int> queue(4);
  // Many times around the ring: masked indexing must stay consistent.
  for (int round = 0; round < 1000; ++round) {
    EXPECT_TRUE(queue.push(int(round)));
    EXPECT_TRUE(queue.push(round + 1000000));
    int a = 0, b = 0;
    ASSERT_TRUE(queue.pop(a));
    ASSERT_TRUE(queue.pop(b));
    EXPECT_EQ(a, round);
    EXPECT_EQ(b, round + 1000000);
  }
  EXPECT_TRUE(queue.empty());
}

TEST(SpscQueueTest, MoveOnlyPayloadsTransferOwnership) {
  SpscQueue<std::unique_ptr<std::string>> queue(4);
  EXPECT_TRUE(queue.push(std::make_unique<std::string>("snapshot")));
  std::unique_ptr<std::string> out;
  ASSERT_TRUE(queue.pop(out));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(*out, "snapshot");
}

// The TSan CI job is the real referee for this test (charter §4.6: the queue
// is the one deliberate cross-thread contact point). The sequence check makes
// it meaningful under plain builds too.
TEST(SpscQueueTest, TwoThreadStressPreservesSequence) {
  constexpr std::uint64_t kCount = 200000;
  SpscQueue<std::uint64_t> queue(1024);

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kCount; ++i) {
      while (!queue.push(std::uint64_t(i))) {
        std::this_thread::yield();  // Full: real producers drop; the test must not lose items.
      }
    }
  });

  std::uint64_t expected = 0;
  bool in_order = true;
  while (expected < kCount) {
    std::uint64_t value = 0;
    if (queue.pop(value)) {
      in_order = in_order && (value == expected);
      ++expected;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
  EXPECT_TRUE(in_order);
  EXPECT_TRUE(queue.empty());
}

// Same stress with an allocating payload: moves through the ring must never
// double-free or leak (ASan job covers this file too).
TEST(SpscQueueTest, TwoThreadStressWithAllocatingPayload) {
  constexpr int kCount = 20000;
  SpscQueue<std::vector<int>> queue(256);

  std::thread producer([&] {
    for (int i = 0; i < kCount; ++i) {
      std::vector<int> item{i, i + 1, i + 2};
      // A failed push leaves `item` untouched (rvalue-ref contract): retry as-is.
      while (!queue.push(std::move(item))) {
        std::this_thread::yield();
      }
    }
  });

  int received = 0;
  bool intact = true;
  while (received < kCount) {
    std::vector<int> item;
    if (queue.pop(item)) {
      intact = intact && item.size() == 3 && item[0] == received && item[2] == received + 2;
      ++received;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
  EXPECT_TRUE(intact);
}

}  // namespace
}  // namespace seashield
