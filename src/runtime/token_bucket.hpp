#pragma once
// Async token bucket (plan doc 10 §4.3): bandwidth / IOPS shaping at the engine entry.
// An unconfigured bucket (rate 0) costs one relaxed atomic load per acquire; a
// configured one refills under a short mutex and parks the coroutine on a reactor
// timer for the shortfall.  Refill time comes from the calling reactor's clock, so
// fake-clock tests drive it deterministically and production reads steady_clock.
//
// configure() may be called at any time (hot reload, plan doc 10 §4.1): the next
// acquire sees the new shape.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

#include "runtime/io.hpp"
#include "runtime/task.hpp"

namespace lnfs::rt {

class TokenBucket {
 public:
  // rate = tokens/second (0 disables); burst = bucket capacity (0 = one second of
  // rate).  Enabling from disabled starts with a full bucket.
  void configure(uint64_t rate_per_s, uint64_t burst = 0) {
    std::lock_guard lock(mu_);
    burst_ = burst ? burst : rate_per_s;
    tokens_ = std::min(tokens_, static_cast<double>(burst_));
    if (rate_.exchange(rate_per_s, std::memory_order_relaxed) == 0) {
      tokens_ = static_cast<double>(burst_);
      last_valid_ = false;  // stamp the refill clock on the next acquire
    }
  }
  uint64_t rate() const { return rate_.load(std::memory_order_relaxed); }

  // Debt-mode acquire: waits until the bucket holds min(n, burst) tokens, then takes
  // all n — the level may go negative, so a request larger than the burst passes (one
  // at a time, at the configured average rate) instead of deadlocking.
  Task<void> acquire(uint64_t n) {
    if (rate_.load(std::memory_order_relaxed) == 0 || n == 0) co_return;
    for (;;) {
      std::chrono::nanoseconds wait{};
      {
        std::lock_guard lock(mu_);
        uint64_t rate = rate_.load(std::memory_order_relaxed);
        if (rate == 0) co_return;  // disabled while we slept
        TimePoint now = current_reactor().now();
        if (last_valid_ && now > last_)
          tokens_ = std::min(tokens_ + std::chrono::duration<double>(now - last_).count() *
                                           static_cast<double>(rate),
                             static_cast<double>(burst_));
        last_ = now;
        last_valid_ = true;
        double need = static_cast<double>(std::min(n, burst_));
        if (tokens_ >= need) {
          tokens_ -= static_cast<double>(n);
          co_return;
        }
        wait = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>((need - tokens_) / static_cast<double>(rate)));
      }
      // Floor the park at 1ms: sub-ms wakeups would burn the reactor for precision
      // no bandwidth cap needs.
      co_await sleep_for(std::max(wait, std::chrono::nanoseconds(1'000'000)));
    }
  }

 private:
  std::mutex mu_;
  std::atomic<uint64_t> rate_{0};
  uint64_t burst_ = 0;   // guarded by mu_
  double tokens_ = 0;    // guarded by mu_
  bool last_valid_ = false;
  TimePoint last_{};
};

}  // namespace lnfs::rt
