#pragma once
// Logging seed (design 08 §8.2 lands the real async structured logger in phase 2; this
// keeps call sites stable: level filter + single write, stderr sink).

#include <atomic>
#include <format>
#include <string_view>

namespace lnfs {

enum class LogLevel : int { kDebug = 0, kInfo, kWarn, kError };

namespace detail {
extern std::atomic<int> g_min_level;
void log_write(LogLevel lv, std::string_view msg);
}  // namespace detail

inline void set_log_level(LogLevel lv) { detail::g_min_level.store(static_cast<int>(lv)); }
inline bool log_enabled(LogLevel lv) {
  return static_cast<int>(lv) >= detail::g_min_level.load(std::memory_order_relaxed);
}

template <class... A>
void log(LogLevel lv, std::format_string<A...> fmt, A&&... a) {
  if (!log_enabled(lv)) return;
  detail::log_write(lv, std::format(fmt, std::forward<A>(a)...));
}

#define LNFS_DEBUG(...) ::lnfs::log(::lnfs::LogLevel::kDebug, __VA_ARGS__)
#define LNFS_INFO(...) ::lnfs::log(::lnfs::LogLevel::kInfo, __VA_ARGS__)
#define LNFS_WARN(...) ::lnfs::log(::lnfs::LogLevel::kWarn, __VA_ARGS__)
#define LNFS_ERROR(...) ::lnfs::log(::lnfs::LogLevel::kError, __VA_ARGS__)

}  // namespace lnfs
