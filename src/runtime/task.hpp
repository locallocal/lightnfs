#pragma once
// Task<T>: lazy, single-consumer coroutine task (design 02 §2.2).
//  - lazily started: runs only when co_awaited (symmetric transfer into the callee)
//  - resumes its awaiter on the same thread (the awaiter's reactor)
//  - move-only; awaiting consumes it (operator co_await on rvalue)
// Exceptions escaping a detached (spawned) task abort the process: fail-fast (02 §2.2).

#include <cassert>
#include <coroutine>
#include <exception>
#include <utility>
#include <variant>

namespace lnfs::rt {

template <class T>
class Task;

namespace detail {

struct FinalAwaiter {
  bool await_ready() noexcept { return false; }
  template <class P>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
    auto cont = h.promise().continuation;
    return cont ? cont : std::noop_coroutine();
  }
  void await_resume() noexcept {}
};

struct PromiseBase {
  std::coroutine_handle<> continuation;
  std::suspend_always initial_suspend() noexcept { return {}; }
  FinalAwaiter final_suspend() noexcept { return {}; }
};

template <class T>
struct TaskPromise : PromiseBase {
  std::variant<std::monostate, T, std::exception_ptr> result;
  Task<T> get_return_object();
  void return_value(T v) { result.template emplace<1>(std::move(v)); }
  void unhandled_exception() { result.template emplace<2>(std::current_exception()); }
  T take() {
    if (result.index() == 2) std::rethrow_exception(std::get<2>(result));
    assert(result.index() == 1);
    return std::move(std::get<1>(result));
  }
};

template <>
struct TaskPromise<void> : PromiseBase {
  std::exception_ptr exc;
  Task<void> get_return_object();
  void return_void() {}
  void unhandled_exception() { exc = std::current_exception(); }
  void take() {
    if (exc) std::rethrow_exception(exc);
  }
};

}  // namespace detail

template <class T>
class [[nodiscard]] Task {
 public:
  using promise_type = detail::TaskPromise<T>;

  Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
  Task(const Task&) = delete;
  Task& operator=(Task&& o) noexcept {
    if (this != &o) {
      if (h_) h_.destroy();
      h_ = std::exchange(o.h_, {});
    }
    return *this;
  }
  ~Task() {
    if (h_) h_.destroy();
  }

  bool valid() const { return static_cast<bool>(h_); }

  auto operator co_await() && {
    struct Awaiter {
      std::coroutine_handle<promise_type> h;
      bool await_ready() const noexcept { return false; }
      std::coroutine_handle<> await_suspend(std::coroutine_handle<> cont) noexcept {
        h.promise().continuation = cont;
        return h;  // symmetric transfer: start the callee
      }
      T await_resume() { return h.promise().take(); }
    };
    assert(h_ && "Task awaited twice or moved-from");
    return Awaiter{h_};
  }

  // Internal: used by spawn() to take ownership of the frame.
  std::coroutine_handle<promise_type> release() { return std::exchange(h_, {}); }

 private:
  friend promise_type;
  explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}
  std::coroutine_handle<promise_type> h_;
};

namespace detail {
template <class T>
Task<T> TaskPromise<T>::get_return_object() {
  return Task<T>(std::coroutine_handle<TaskPromise<T>>::from_promise(*this));
}
inline Task<void> TaskPromise<void>::get_return_object() {
  return Task<void>(std::coroutine_handle<TaskPromise<void>>::from_promise(*this));
}
}  // namespace detail

}  // namespace lnfs::rt
