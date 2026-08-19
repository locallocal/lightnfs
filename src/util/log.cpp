#include "util/log.hpp"

#include <cstdio>
#include <ctime>
#include <mutex>

namespace lnfs::detail {

std::atomic<int> g_min_level{static_cast<int>(lnfs::LogLevel::kInfo)};

static const char* level_name(LogLevel lv) {
  switch (lv) {
    case LogLevel::kDebug: return "debug";
    case LogLevel::kInfo: return "info";
    case LogLevel::kWarn: return "warn";
    case LogLevel::kError: return "error";
  }
  return "?";
}

void log_write(LogLevel lv, std::string_view msg) {
  static std::mutex mu;
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  std::lock_guard lk(mu);
  std::fprintf(stderr, "ts=%lld.%03ld level=%s %.*s\n", static_cast<long long>(ts.tv_sec),
               ts.tv_nsec / 1000000, level_name(lv), static_cast<int>(msg.size()), msg.data());
}

}  // namespace lnfs::detail
