#include "core/logger.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace seashield::log {

namespace {

std::atomic<int> g_min_level{static_cast<int>(Level::kInfo)};

const char* level_tag(Level level) {
  switch (level) {
    case Level::kDebug:
      return "DEBUG";
    case Level::kInfo:
      return "INFO";
    case Level::kWarn:
      return "WARN";
    case Level::kError:
      return "ERROR";
  }
  return "?";
}

}  // namespace

void set_min_level(Level level) { g_min_level.store(static_cast<int>(level), std::memory_order_relaxed); }

void write(Level level, const char* fmt, ...) {
  if (static_cast<int>(level) < g_min_level.load(std::memory_order_relaxed)) {
    return;
  }

  char message[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  using std::chrono::duration_cast;
  using std::chrono::milliseconds;
  using std::chrono::system_clock;
  const auto now = system_clock::now();
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  const std::time_t seconds = system_clock::to_time_t(now);
  std::tm tm_buf{};
  ::localtime_r(&seconds, &tm_buf);
  char timestamp[16];
  std::strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &tm_buf);

  // Single fprintf call keeps one log line contiguous across threads.
  std::fprintf(stderr, "%s.%03d %-5s %s\n", timestamp, static_cast<int>(ms), level_tag(level),
               message);
}

}  // namespace seashield::log
