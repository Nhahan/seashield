#include "net/event_loop.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "core/unique_fd.h"

namespace seashield::net {
namespace {

using namespace std::chrono;
using namespace std::chrono_literals;

class EventLoopTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = EventLoop::create();
    ASSERT_NE(loop_, nullptr);
    int raw[2] = {-1, -1};
    ASSERT_EQ(::pipe(raw), 0);
    read_end_.reset(raw[0]);
    write_end_.reset(raw[1]);
  }

  void write_byte() {
    const char byte = 'x';
    ASSERT_EQ(::write(write_end_.get(), &byte, 1), 1);
  }

  std::unique_ptr<EventLoop> loop_;
  UniqueFd read_end_;
  UniqueFd write_end_;
};

TEST_F(EventLoopTest, TimeoutReturnsZero) {
  const auto start = steady_clock::now();
  EXPECT_EQ(loop_->run_once(20), 0);
  EXPECT_LT(steady_clock::now() - start, 2s);
}

TEST_F(EventLoopTest, DispatchesReadOnPipe) {
  unsigned seen = 0;
  ASSERT_TRUE(loop_->add(read_end_.get(), IoEvents::kRead, [&](unsigned ev) { seen = ev; }));
  write_byte();
  EXPECT_GE(loop_->run_once(1000), 1);
  EXPECT_NE(seen & IoEvents::kRead, 0u);
}

TEST_F(EventLoopTest, WriteInterestFiresWhenWritable) {
  unsigned seen = 0;
  ASSERT_TRUE(loop_->add(write_end_.get(), IoEvents::kWrite, [&](unsigned ev) { seen = ev; }));
  EXPECT_GE(loop_->run_once(1000), 1);
  EXPECT_NE(seen & IoEvents::kWrite, 0u);
}

TEST_F(EventLoopTest, ModifyToEmptyInterestSilencesFd) {
  int calls = 0;
  ASSERT_TRUE(loop_->add(read_end_.get(), IoEvents::kRead, [&](unsigned) { ++calls; }));
  write_byte();
  EXPECT_GE(loop_->run_once(1000), 1);
  EXPECT_EQ(calls, 1);

  ASSERT_TRUE(loop_->modify(read_end_.get(), 0));
  write_byte();
  EXPECT_EQ(loop_->run_once(50), 0);
  EXPECT_EQ(calls, 1);

  // Re-enabling read interest delivers the pending data again (level trigger).
  ASSERT_TRUE(loop_->modify(read_end_.get(), IoEvents::kRead));
  EXPECT_GE(loop_->run_once(1000), 1);
  EXPECT_EQ(calls, 2);
}

TEST_F(EventLoopTest, RemoveStopsCallbacks) {
  int calls = 0;
  ASSERT_TRUE(loop_->add(read_end_.get(), IoEvents::kRead, [&](unsigned) { ++calls; }));
  ASSERT_TRUE(loop_->remove(read_end_.get()));
  write_byte();
  EXPECT_EQ(loop_->run_once(50), 0);
  EXPECT_EQ(calls, 0);
}

TEST_F(EventLoopTest, CallbackCanRemoveItself) {
  int calls = 0;
  ASSERT_TRUE(loop_->add(read_end_.get(), IoEvents::kRead, [&](unsigned) {
    ++calls;
    EXPECT_TRUE(loop_->remove(read_end_.get()));
  }));
  write_byte();
  EXPECT_GE(loop_->run_once(1000), 1);
  EXPECT_EQ(calls, 1);

  write_byte();
  EXPECT_EQ(loop_->run_once(50), 0);
  EXPECT_EQ(calls, 1);
}

TEST_F(EventLoopTest, DoubleAddIsRejected) {
  ASSERT_TRUE(loop_->add(read_end_.get(), IoEvents::kRead, [](unsigned) {}));
  EXPECT_FALSE(loop_->add(read_end_.get(), IoEvents::kRead, [](unsigned) {}));
}

TEST_F(EventLoopTest, WakeupUnblocksFromAnotherThread) {
  std::atomic<bool> woke{false};
  std::thread waker([&] {
    std::this_thread::sleep_for(50ms);
    woke.store(true);
    loop_->wakeup();
  });

  const auto start = steady_clock::now();
  const int dispatched = loop_->run_once(5000);
  const auto elapsed = steady_clock::now() - start;
  waker.join();

  EXPECT_GE(dispatched, 1);
  EXPECT_TRUE(woke.load());
  EXPECT_LT(elapsed, 3s);
}

}  // namespace
}  // namespace seashield::net
