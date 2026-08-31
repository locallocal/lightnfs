#pragma once
// Offload pool (design 02 §2.2/§2.3): the single cross-thread point of the runtime. Blocking
// syscalls without uring support (openat/rename/getdents...) and blocking backend libraries
// run here; completion resumes the awaiting coroutine back on its originating reactor.
//
// Structure (plan doc 10 §2.5, replacing the single-lock unbounded FIFO):
//  - two job classes: kLight (statx/open/readdir-grade metadata) and kHeavy
//    (fsync/fallocate/copy-grade). Heavy jobs run only on a reserved slice of the
//    threads, so a burst of slow fsyncs can never head-of-line-block metadata.
//  - per-class sharded queues (one shard per worker of that class) + a counting
//    semaphore for parking. submit() touches one shard lock; an idle worker scans all
//    shards of its class starting at its own (that scan is the work stealing).
//  - admission cap per class: beyond `queue_cap` queued jobs, submissions wait in an
//    overflow queue that refills the shards as workers drain them, so the shard queues
//    (and thus completion latency for admitted jobs) stay bounded.
//  - queue depth / totals exported via stats() (design 08 §8.3).

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <thread>
#include <type_traits>
#include <vector>

#include "runtime/reactor.hpp"

namespace lnfs::rt {

class MoveOnlyFn {
  struct Base {
    virtual void call() = 0;
    virtual ~Base() = default;
  };
  template <class F>
  struct Impl final : Base {
    F f;
    explicit Impl(F&& f_) : f(std::move(f_)) {}
    void call() override { f(); }
  };
  std::unique_ptr<Base> p_;

 public:
  MoveOnlyFn() = default;
  template <class F>
  MoveOnlyFn(F f) : p_(std::make_unique<Impl<F>>(std::move(f))) {}  // NOLINT
  void operator()() { p_->call(); }
  explicit operator bool() const { return p_ != nullptr; }
};

enum class OffloadClass : uint8_t { kLight = 0, kHeavy = 1 };
inline constexpr int kOffloadClasses = 2;

class OffloadPool {
 public:
  struct Config {
    int threads = 8;
    // 0 = max(1, threads / 4). Heavy jobs are confined to these threads; when idle
    // they park rather than steal light work (predictability over utilization).
    int heavy_threads = 0;
    size_t queue_cap = 4096;  // per class: queued-jobs bound before admission holds
  };

  struct Stats {
    uint64_t submitted[kOffloadClasses]{};
    uint64_t completed[kOffloadClasses]{};
    uint64_t deferred[kOffloadClasses]{};  // held in admission overflow at least once
    size_t depth[kOffloadClasses]{};       // queued: admitted + overflow (design 08 §8.3)
  };

  explicit OffloadPool(int threads) : OffloadPool(Config{.threads = threads}) {}
  explicit OffloadPool(Config cfg);
  ~OffloadPool();

  void submit(MoveOnlyFn job, OffloadClass cls = OffloadClass::kLight);
  Stats stats() const;
  void stop_and_join();

 private:
  struct Shard {
    std::mutex mu;
    std::deque<MoveOnlyFn> q;
  };
  // One class's worker group: `shards.size()` == its worker count.
  struct Group {
    std::vector<std::unique_ptr<Shard>> shards;
    std::counting_semaphore<> sem{0};
    std::atomic<uint64_t> rr{0};
    std::atomic<size_t> depth{0};
    std::atomic<uint64_t> submitted{0}, completed{0}, deferred{0};
    mutable std::mutex overflow_mu;
    std::deque<MoveOnlyFn> overflow;
    size_t queue_cap = 0;
  };

  // The group serving this class; a heavy group with no workers (1-thread pool)
  // falls back to the light group.
  Group& group_for(OffloadClass cls) {
    Group& g = groups_[static_cast<int>(cls)];
    return g.shards.empty() ? groups_[0] : g;
  }
  void worker(Group& g, size_t idx);
  void enqueue(Group& g, MoveOnlyFn job);
  bool try_pop(Group& g, size_t start, MoveOnlyFn& out);
  void refill_from_overflow(Group& g);

  Group groups_[kOffloadClasses];
  std::vector<std::thread> threads_;
  std::atomic<bool> stopping_{false};
};

// co_await offload([...]{ blocking work; return v; }, cls) — runs fn on the pool, resumes
// on the originating reactor with fn's result. Exceptions propagate to the awaiter.
// kHeavy marks jobs that can stall on storage (fsync/fallocate/copy) so they cannot
// head-of-line-block metadata ops (plan doc 10 §2.5).
template <class F>
auto offload(F fn, OffloadClass cls = OffloadClass::kLight) {
  using R = std::invoke_result_t<F>;
  struct Awaiter {
    F fn;
    OffloadClass cls;
    Reactor* r = nullptr;
    std::coroutine_handle<> h{};
    std::exception_ptr exc{};
    // storage for non-void results
    std::conditional_t<std::is_void_v<R>, char, std::optional<R>> res{};

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h_) {
      h = h_;
      r = &current_reactor();
      OffloadPool* pool = r->offload_pool();
      if (!pool) {
        std::fprintf(stderr, "lnfs: offload() with no OffloadPool attached to reactor\n");
        std::abort();
      }
      pool->submit(
          [this] {
            try {
              if constexpr (std::is_void_v<R>) fn();
              else res.emplace(fn());
            } catch (...) {
              exc = std::current_exception();
            }
            // After post() the frame may resume and die concurrently: copy first, then post.
            Reactor* rr = r;
            auto hh = h;
            rr->post(hh);
          },
          cls);
    }
    R await_resume() {
      if (exc) std::rethrow_exception(exc);
      if constexpr (!std::is_void_v<R>) return std::move(*res);
    }
  };
  return Awaiter{std::move(fn), cls};
}

}  // namespace lnfs::rt
