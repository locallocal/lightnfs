#pragma once
// Reactor (design 02 §2.3): submit SQEs -> wait CQEs -> resume waiting coroutines -> run
// expired timers. One reactor per thread; connections are pinned to a reactor, so business
// code above the runtime is effectively single-threaded except for explicitly sharded state.
//
// Cross-thread entry points are exactly two (02 §2.3): post() (remote wakeup, used by
// offload completion and sync primitives) and spawn_on() (top-level task placement).

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <vector>

#include "runtime/ring_ops.hpp"
#include "runtime/task.hpp"

namespace lnfs::rt {

class OffloadPool;

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// MPSC handoff for cross-thread wakeups. Mutex + swap; contention is low (design allows
// starting simple; the interface is what matters).
class MpscQueue {
 public:
  void push(std::coroutine_handle<> h) {
    std::lock_guard lk(mu_);
    q_.push_back(h);
  }
  // Swaps the queue into `out` (cleared first).  The caller keeps reusing the same
  // buffer, so the two vectors ping-pong their capacity and steady-state drains never
  // allocate (plan doc 10 §2.2 — the old local-vector pattern freed the capacity of
  // both sides every pump).
  void drain(std::vector<std::coroutine_handle<>>& out) {
    out.clear();
    std::lock_guard lk(mu_);
    out.swap(q_);
  }
  bool empty() {
    std::lock_guard lk(mu_);
    return q_.empty();
  }

 private:
  std::mutex mu_;
  std::vector<std::coroutine_handle<>> q_;
};

class Reactor {
 public:
  struct Options {
    // Injectable clock so FakeRing tests can drive timers deterministically.
    // Null means steady_clock.
    std::function<TimePoint()> clock;
  };

  explicit Reactor(RingOps& ring, Options opts = {});
  ~Reactor();

  Reactor(const Reactor&) = delete;

  // Runs until stop() AND all spawned tasks have finished.
  void run();
  void stop();  // thread-safe

  // Thread-safe: schedule h to be resumed on this reactor's thread.  Calls from this
  // reactor's own thread take a syscall-free fast path (plan doc 10 §2.2): the handle
  // goes to a plain local queue instead of the locked MPSC queue + eventfd wake.
  void post(std::coroutine_handle<> h);

  RingOps& ring() { return ring_; }
  OffloadPool* offload_pool() const { return offload_; }
  void set_offload_pool(OffloadPool* p) { offload_ = p; }
  TimePoint now() const { return opts_.clock ? opts_.clock() : Clock::now(); }

  // ---- io / timer plumbing (called from this reactor's thread) ----
  void op_started() { ++pending_ops_; }
  void op_finished() { --pending_ops_; }
  void add_timer(TimePoint deadline, std::coroutine_handle<> h);

  // ---- spawn bookkeeping ----
  void task_started() { live_tasks_.fetch_add(1, std::memory_order_relaxed); }
  void task_finished() { live_tasks_.fetch_sub(1, std::memory_order_relaxed); }
  int64_t live_tasks() const { return live_tasks_.load(std::memory_order_relaxed); }

  // Test helper: process what is ready without blocking; returns true if progress was made.
  bool poll_once();

  // Loop busy-period stats (plan doc 10 §3.5): each run() sweep that made progress
  // records how long the reactor was busy — the delay newly arriving work experiences.
  // Written relaxed from the reactor thread only; readable from any thread (metrics).
  static constexpr uint64_t kLoopBoundsUs[] = {10,   50,    100,   500,
                                               1000, 5000,  10000, 50000};
  static constexpr size_t kLoopBuckets = std::size(kLoopBoundsUs) + 1;
  struct LoopStats {
    uint64_t buckets[kLoopBuckets]{};
    uint64_t sum_us = 0;
    uint64_t count = 0;
  };
  LoopStats loop_stats() const {
    LoopStats out;
    for (size_t b = 0; b < kLoopBuckets; ++b) {
      out.buckets[b] = loop_buckets_[b].load(std::memory_order_relaxed);
      out.count += out.buckets[b];
    }
    out.sum_us = loop_sum_us_.load(std::memory_order_relaxed);
    return out;
  }

 private:
  bool pump(std::optional<std::chrono::nanoseconds> block_for);
  std::optional<std::chrono::nanoseconds> next_timer_delay();
  void run_expired_timers();
  void observe_loop(uint64_t us) {
    size_t b = 0;
    while (b < kLoopBuckets - 1 && us > kLoopBoundsUs[b]) ++b;
    loop_buckets_[b].fetch_add(1, std::memory_order_relaxed);
    loop_sum_us_.fetch_add(us, std::memory_order_relaxed);
  }

  RingOps& ring_;
  Options opts_;
  OffloadPool* offload_ = nullptr;

  struct TimerEnt {
    TimePoint deadline;
    uint64_t seq;  // FIFO tie-break
    std::coroutine_handle<> h;
    bool operator>(const TimerEnt& o) const {
      return deadline != o.deadline ? deadline > o.deadline : seq > o.seq;
    }
  };
  std::priority_queue<TimerEnt, std::vector<TimerEnt>, std::greater<>> timers_;
  uint64_t timer_seq_ = 0;

  MpscQueue remote_;
  // Same-thread post() targets; drained by pump() with no lock or wake involved.
  // A deque so handles queued by a resumed handle land behind the current batch.
  std::deque<std::coroutine_handle<>> local_ready_;
  std::vector<std::coroutine_handle<>> drain_buf_;  // reused across pumps (see drain())
  std::atomic<bool> stop_{false};
  std::atomic<int64_t> live_tasks_{0};
  int64_t pending_ops_ = 0;
  std::atomic<uint64_t> loop_buckets_[kLoopBuckets]{};
  std::atomic<uint64_t> loop_sum_us_{0};
};

// TLS accessor (design 02 §2.2). Aborts if called off-reactor.
Reactor& current_reactor();
Reactor* current_reactor_or_null();
// Test/bench helper: mark the calling thread as running `r` (normally done by run()).
class ReactorGuard {
 public:
  explicit ReactorGuard(Reactor* r);
  ~ReactorGuard();

 private:
  Reactor* prev_;
};

// Detached start of a top-level coroutine on reactor r (design 02 §2.2).
void spawn(Task<void> t, Reactor& r);
inline void spawn_on(Reactor& r, Task<void> t) { spawn(std::move(t), r); }

}  // namespace lnfs::rt
