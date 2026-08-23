#pragma once
// Logging facade (design 08 §8.2) — implemented by spdlog (third_party/spdlog
// submodule). Call sites keep the LNFS_* macros with fmt-style placeholders; the sink
// is stderr with the logfmt-ish `ts=<epoch>.<ms> level=<lv> <msg>` pattern.
// init_async_logging() switches the production main to spdlog's async logger (bounded
// queue drained by one worker, drop-newest on overflow so a stalled sink never stalls
// reactors); tests, tools and benches stay on the synchronous stderr default.

#include <spdlog/spdlog.h>

namespace lnfs {

enum class LogLevel : int { kDebug = 0, kInfo, kWarn, kError };

namespace detail {
constexpr spdlog::level::level_enum to_spdlog(LogLevel lv) {
  switch (lv) {
    case LogLevel::kDebug: return spdlog::level::debug;
    case LogLevel::kWarn: return spdlog::level::warn;
    case LogLevel::kError: return spdlog::level::err;
    default: return spdlog::level::info;
  }
}
// Returns the current logger, installing the synchronous stderr one on first touch.
spdlog::logger* logger();
}  // namespace detail

inline void set_log_level(LogLevel lv) {
  detail::logger()->set_level(detail::to_spdlog(lv));
}
inline bool log_enabled(LogLevel lv) {
  return detail::logger()->should_log(detail::to_spdlog(lv));
}

template <class... A>
void log(LogLevel lv, spdlog::format_string_t<A...> fmt, A&&... a) {
  detail::logger()->log(detail::to_spdlog(lv), fmt, std::forward<A>(a)...);
}

// Switch the sink to the async logger (production main); keeps the current level.
void init_async_logging();
void shutdown_async_logging();

#define LNFS_DEBUG(...) ::lnfs::log(::lnfs::LogLevel::kDebug, __VA_ARGS__)
#define LNFS_INFO(...) ::lnfs::log(::lnfs::LogLevel::kInfo, __VA_ARGS__)
#define LNFS_WARN(...) ::lnfs::log(::lnfs::LogLevel::kWarn, __VA_ARGS__)
#define LNFS_ERROR(...) ::lnfs::log(::lnfs::LogLevel::kError, __VA_ARGS__)

}  // namespace lnfs
