#pragma once

#include <unistd.h>

#include <utility>

namespace seashield {

// RAII owner of a POSIX file descriptor. Move-only; closes on destruction.
class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() { reset(); }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  int get() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }

  // Gives up ownership without closing.
  int release() noexcept { return std::exchange(fd_, -1); }

  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

}  // namespace seashield
