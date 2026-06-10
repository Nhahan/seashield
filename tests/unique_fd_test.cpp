#include "core/unique_fd.h"

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace seashield {
namespace {

TEST(UniqueFdTest, DefaultIsInvalid) {
  UniqueFd fd;
  EXPECT_FALSE(fd.valid());
  EXPECT_EQ(fd.get(), -1);
}

TEST(UniqueFdTest, ClosesOnDestruction) {
  int raw[2] = {-1, -1};
  ASSERT_EQ(::pipe(raw), 0);
  {
    UniqueFd read_end(raw[0]);
    UniqueFd write_end(raw[1]);
    EXPECT_TRUE(read_end.valid());
  }
  // Both ends must be closed now: fcntl on a closed fd fails with EBADF.
  EXPECT_EQ(::fcntl(raw[0], F_GETFD), -1);
  EXPECT_EQ(::fcntl(raw[1], F_GETFD), -1);
}

TEST(UniqueFdTest, MoveTransfersOwnership) {
  int raw[2] = {-1, -1};
  ASSERT_EQ(::pipe(raw), 0);
  UniqueFd a(raw[0]);
  UniqueFd b(std::move(a));
  EXPECT_FALSE(a.valid());
  EXPECT_EQ(b.get(), raw[0]);

  UniqueFd c(raw[1]);
  c = std::move(b);  // Closes raw[1], takes raw[0].
  EXPECT_EQ(c.get(), raw[0]);
  EXPECT_EQ(::fcntl(raw[1], F_GETFD), -1);
}

TEST(UniqueFdTest, ReleaseGivesUpOwnership) {
  int raw[2] = {-1, -1};
  ASSERT_EQ(::pipe(raw), 0);
  UniqueFd write_end(raw[1]);
  {
    UniqueFd fd(raw[0]);
    EXPECT_EQ(fd.release(), raw[0]);
    EXPECT_FALSE(fd.valid());
  }
  // Released fd must still be open.
  EXPECT_NE(::fcntl(raw[0], F_GETFD), -1);
  ::close(raw[0]);
}

}  // namespace
}  // namespace seashield
