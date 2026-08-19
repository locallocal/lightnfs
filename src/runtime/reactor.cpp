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

  std::vector<std::coroutine_handle<>> ready;
  remote_.drain(ready);
  for (auto h : ready) {
    progress = true;
    h.resume();
  }

  run_expired_timers();

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
  return pump(std::chrono::nanoseconds(0));
}

void Reactor::run() {
  ReactorGuard g(this);
  for (;;) {
    // Non-blocking sweep until quiescent, then decide whether to exit or block.
    while (pump(std::chrono::nanoseconds(0))) {
    }
    if (stop_.load(std::memory_order_acquire) && live_tasks() == 0 && pending_ops_ == 0 &&
        remote_.empty()) {
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
