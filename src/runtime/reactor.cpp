#include "runtime/reactor.hpp"

#include <cstdio>
#include <cstdlib>

namespace lnfs::rt {

namespace {
thread_local Reactor* t_current = nullptr;
}  // namespace

Reactor& current_reactor() {
  if (!t_current) {
    std::fprintf(stderr, "lnfs: current_reactor() called off-reactor\n");
    std::abort();
  }
  return *t_current;
}
Reactor* current_reactor_or_null() { return t_current; }

ReactorGuard::ReactorGuard(Reactor* r) : prev_(t_current) { t_current = r; }
ReactorGuard::~ReactorGuard() { t_current = prev_; }

Reactor::Reactor(RingOps& ring, Options opts) : ring_(ring), opts_(std::move(opts)) {}
Reactor::~Reactor() = default;

void Reactor::post(std::coroutine_handle<> h) {
  // Same-thread fast path (plan doc 10 §2.2): the hot paths — per-request spawn, every
  // sync-primitive handoff, the reply write lock — post from the reactor's own thread,
  // where a locked queue + eventfd write (a real syscall) buys nothing.  Queueing
  // instead of resuming inline also keeps resume depth bounded.
  if (current_reactor_or_null() == this) {
    local_ready_.push_back(h);
    return;
  }
  remote_.push(h);
  ring_.wake();
}

void Reactor::stop() {
  stop_.store(true);
  ring_.wake();
}

void Reactor::add_timer(TimePoint deadline, std::coroutine_handle<> h) {
  timers_.push(TimerEnt{deadline, timer_seq_++, h});
}

void Reactor::run_expired_timers() {
  const TimePoint t = now();
  while (!timers_.empty() && timers_.top().deadline <= t) {
    auto h = timers_.top().h;
    timers_.pop();
    h.resume();
  }
}

std::optional<std::chrono::nanoseconds> Reactor::next_timer_delay() {
  if (timers_.empty()) return std::nullopt;
  auto d = timers_.top().deadline - now();
  if (d.count() < 0) d = {};
  return std::chrono::duration_cast<std::chrono::nanoseconds>(d);
}

bool Reactor::pump(std::optional<std::chrono::nanoseconds> block_for) {
  bool progress = false;

  remote_.drain(drain_buf_);
  for (auto h : drain_buf_) {
    progress = true;
    h.resume();
  }

  // Same-thread posts: run the batch queued before this pump; what a resumed handle
  // posts now runs next round, mirroring how the remote queue snapshots.
  for (size_t k = local_ready_.size(); k > 0; --k) {
    auto h = local_ready_.front();
    local_ready_.pop_front();
    progress = true;
    h.resume();
  }

  run_expired_timers();

  // Handles resumed above can arm new timers; blocking on a timeout computed before
  // they ran would sleep straight past those deadlines (a post is covered by the ring's
  // wake fd, but a timer touches nothing the ring can see). Re-clamp to the nearest
  // deadline as it stands now.
  if (auto d = next_timer_delay(); d && (!block_for || *d < *block_for)) block_for = *d;

  // Ready local work must never sit behind a blocking wait — nothing wakes the ring
  // for a same-thread post.
  if (!local_ready_.empty()) block_for = std::chrono::nanoseconds(0);

  Completion comps[256];
  size_t n = ring_.wait(std::span<Completion>(comps), block_for);
  for (size_t i = 0; i < n; ++i) {
    progress = true;
    comps[i].op->res = comps[i].res;
    --pending_ops_;
    comps[i].op->waiter.resume();
  }
  run_expired_timers();
  return progress;
}

bool Reactor::poll_once() {
  ReactorGuard g(this);
  ring_.bind_submitter();
  return pump(std::chrono::nanoseconds(0));
}

void Reactor::run() {
  ReactorGuard g(this);
  // First act on the reactor thread: claim the ring as its single issuer (io_uring
  // SINGLE_ISSUER/DEFER_TASKRUN setup, plan doc 10 §2.3). No-op for other backends.
  ring_.bind_submitter();
  for (;;) {
    // Non-blocking sweep until quiescent, then decide whether to exit or block.
    // The sweep duration is the loop busy period (plan doc 10 §3.5).
    auto t0 = Clock::now();
    bool busy = false;
    while (pump(std::chrono::nanoseconds(0))) busy = true;
    if (busy)
      observe_loop(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0)
              .count()));
    if (stop_.load(std::memory_order_acquire) && live_tasks() == 0 && pending_ops_ == 0 &&
        remote_.empty() && local_ready_.empty()) {
      break;
    }
    pump(next_timer_delay());
  }
}

// ---- spawn ----------------------------------------------------------------

namespace {

// Self-destroying root coroutine that owns a detached Task (design 02 §2.2: uncaught
// exceptions in a task chain are fatal — fail-fast, never silently wrong).
struct SpawnRoot {
  struct promise_type {
    SpawnRoot get_return_object() {
      return {std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {
      std::fprintf(stderr, "lnfs: uncaught exception escaped a spawned task; aborting\n");
      std::abort();
    }
    static void* operator new(size_t n) { return detail::frame_alloc(n); }
    static void operator delete(void* p, size_t n) noexcept { detail::frame_free(p, n); }
  };
  std::coroutine_handle<promise_type> h;
};

SpawnRoot spawn_root(Task<void> t, Reactor* r) {
  co_await std::move(t);
  r->task_finished();
}

}  // namespace

void spawn(Task<void> t, Reactor& r) {
  r.task_started();
  auto root = spawn_root(std::move(t), &r);
  r.post(root.h);
}

}  // namespace lnfs::rt
