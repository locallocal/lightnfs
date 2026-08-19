#pragma once
// Offload pool (design 02 §2.2/§2.3): the single cross-thread point of the runtime. Blocking
// syscalls without uring support (openat/rename/getdents...) and blocking backend libraries
// run here; completion resumes the awaiting coroutine back on its originating reactor.

#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
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

class OffloadPool {
 public:
  explicit OffloadPool(int threads);
  ~OffloadPool();
  void submit(MoveOnlyFn job);
  size_t queue_depth();
  void stop_and_join();

 private:
  void worker();
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<MoveOnlyFn> q_;
  std::vector<std::thread> threads_;
  bool stopping_ = false;
};

// co_await offload([...]{ blocking work; return v; }) — runs fn on the pool, resumes on the
// originating reactor with fn's result. Exceptions propagate to the awaiter.
template <class F>
auto offload(F fn) {
  using R = std::invoke_result_t<F>;
  struct Awaiter {
    F fn;
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
      pool->submit([this] {
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
      });
    }
    R await_resume() {
      if (exc) std::rethrow_exception(exc);
      if constexpr (!std::is_void_v<R>) return std::move(*res);
    }
  };
  return Awaiter{std::move(fn)};
}

}  // namespace lnfs::rt
