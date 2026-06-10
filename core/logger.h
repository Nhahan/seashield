#pragma once

namespace seashield::log {

enum class Level { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

void set_min_level(Level level);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void write(Level level, const char* fmt, ...);

}  // namespace seashield::log

#define SS_LOG_DEBUG(...) ::seashield::log::write(::seashield::log::Level::kDebug, __VA_ARGS__)
#define SS_LOG_INFO(...) ::seashield::log::write(::seashield::log::Level::kInfo, __VA_ARGS__)
#define SS_LOG_WARN(...) ::seashield::log::write(::seashield::log::Level::kWarn, __VA_ARGS__)
#define SS_LOG_ERROR(...) ::seashield::log::write(::seashield::log::Level::kError, __VA_ARGS__)
