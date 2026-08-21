#include "util/log.hpp"

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

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

// Async sink (design 08 §8.2): fixed-slot ring drained by one flush thread, so the hot
// path is one format into a stack buffer plus a memcpy under a short lock.  Overflow
// drops the newest entry and counts it (a stalled disk must not stall reactors).
namespace {

struct Slot {
  LogLevel level;
  timespec ts;
  uint16_t len;
  char text[240];
};

constexpr size_t kSlots = 8192;

struct AsyncSink {
  std::mutex mu;
  std::condition_variable cv;
  Slot slots[kSlots];
  size_t head = 0, tail = 0;  // ring: [tail, head)
  uint64_t dropped = 0;
  bool stop = false;
  std::thread flusher;

  void run() {
    std::unique_lock lock(mu);
    for (;;) {
      cv.wait(lock, [&] { return head != tail || stop; });
      while (head != tail) {
        Slot local = slots[tail];
        tail = (tail + 1) % kSlots;
        uint64_t lost = dropped;
        dropped = 0;
        lock.unlock();
        std::fprintf(stderr, "ts=%lld.%03ld level=%s %.*s\n",
                     static_cast<long long>(local.ts.tv_sec), local.ts.tv_nsec / 1000000,
                     level_name(local.level), static_cast<int>(local.len), local.text);
        if (lost > 0)
          std::fprintf(stderr, "ts=%lld.000 level=warn log ring overflow: %llu dropped\n",
                       static_cast<long long>(local.ts.tv_sec),
                       static_cast<unsigned long long>(lost));
        lock.lock();
      }
      if (stop) return;
    }
  }
};

AsyncSink* g_async = nullptr;

}  // namespace

void log_write(LogLevel lv, std::string_view msg) {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  if (AsyncSink* sink = g_async) {
    std::lock_guard lock(sink->mu);
    size_t next = (sink->head + 1) % kSlots;
    if (next == sink->tail) {
      ++sink->dropped;
      return;
    }
    Slot& slot = sink->slots[sink->head];
    slot.level = lv;
    slot.ts = ts;
    slot.len = static_cast<uint16_t>(std::min(msg.size(), sizeof(slot.text)));
    std::memcpy(slot.text, msg.data(), slot.len);
    sink->head = next;
    sink->cv.notify_one();
    return;
  }
  static std::mutex mu;
  std::lock_guard lk(mu);
  std::fprintf(stderr, "ts=%lld.%03ld level=%s %.*s\n", static_cast<long long>(ts.tv_sec),
               ts.tv_nsec / 1000000, level_name(lv), static_cast<int>(msg.size()), msg.data());
}

}  // namespace lnfs::detail

namespace lnfs {

void init_async_logging() {
  using detail::g_async;
  if (g_async) return;
  auto* sink = new detail::AsyncSink();
  sink->flusher = std::thread([sink] { sink->run(); });
  g_async = sink;
}

void shutdown_async_logging() {
  using detail::g_async;
  detail::AsyncSink* sink = g_async;
  if (!sink) return;
  g_async = nullptr;  // subsequent writes go synchronous
  {
    std::lock_guard lock(sink->mu);
    sink->stop = true;
  }
  sink->cv.notify_one();
  sink->flusher.join();
  delete sink;
}

}  // namespace lnfs
