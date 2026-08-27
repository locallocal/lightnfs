#pragma once
// Awaitable io primitives (design 02 §2.2). All return the io_uring result convention:
// >= 0 success, < 0 negated errno; upper layers wrap into Result<T>.
//
// Usage rules:
//  - must be co_awaited from a reactor coroutine, in the same full expression they are
//    created in (the awaiter lives in the caller frame while the op is in flight);
//  - pointer arguments (paths, statx out, iovecs) must stay alive until the await returns.

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <optional>
#include <span>

#include "runtime/reactor.hpp"
#include "runtime/ring_ops.hpp"
#include "runtime/task.hpp"

namespace lnfs::rt {

namespace detail {

template <class Prep>
struct IoAwaiter {
  Prep prep;
  OpHandle op{};
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) {
    op.waiter = h;
    Reactor& r = current_reactor();
    r.op_started();
    prep(r.ring(), &op);
  }
  int32_t await_resume() noexcept { return op.res; }
};
template <class Prep>
IoAwaiter(Prep) -> IoAwaiter<Prep>;

}  // namespace detail

inline auto uring_read(int fd, std::span<std::byte> buf, uint64_t off) {
  return detail::IoAwaiter{
      [=](RingOps& ring, OpHandle* op) { ring.prep_read(op, fd, buf, off); }};
}
inline auto uring_write(int fd, std::span<const std::byte> buf, uint64_t off) {
  return detail::IoAwaiter{
      [=](RingOps& ring, OpHandle* op) { ring.prep_write(op, fd, buf, off); }};
}
inline auto uring_writev(int fd, const iovec* iov, int iovcnt, uint64_t off) {
  return detail::IoAwaiter{
      [=](RingOps& ring, OpHandle* op) { ring.prep_writev(op, fd, iov, iovcnt, off); }};
}
inline auto uring_fsync(int fd, bool datasync) {
  return detail::IoAwaiter{
      [=](RingOps& ring, OpHandle* op) { ring.prep_fsync(op, fd, datasync); }};
}
inline auto uring_recv(int fd, std::span<std::byte> buf) {
  return detail::IoAwaiter{[=](RingOps& ring, OpHandle* op) { ring.prep_recv(op, fd, buf); }};
}
inline auto uring_sendv(int fd, const iovec* iov, int iovcnt) {
  return detail::IoAwaiter{
      [=](RingOps& ring, OpHandle* op) { ring.prep_sendv(op, fd, iov, iovcnt); }};
}
inline auto uring_accept(int fd, sockaddr* addr, socklen_t* alen) {
  return detail::IoAwaiter{
      [=](RingOps& ring, OpHandle* op) { ring.prep_accept(op, fd, addr, alen); }};
}
inline auto uring_statx(int dirfd, const char* path, int flags, unsigned mask,
                        struct statx* out) {
  return detail::IoAwaiter{
      [=](RingOps& ring, OpHandle* op) { ring.prep_statx(op, dirfd, path, flags, mask, out); }};
}
inline auto uring_openat(int dirfd, const char* path, int flags, mode_t mode) {
  return detail::IoAwaiter{
      [=](RingOps& ring, OpHandle* op) { ring.prep_openat(op, dirfd, path, flags, mode); }};
}
inline auto uring_close(int fd) {
  return detail::IoAwaiter{[=](RingOps& ring, OpHandle* op) { ring.prep_close(op, fd); }};
}
// Best-effort cancellation of in-flight ops on fd (design 02 §2.6).
inline auto uring_cancel_fd(int fd) {
  return detail::IoAwaiter{[=](RingOps& ring, OpHandle* op) { ring.prep_cancel_fd(op, fd); }};
}

// ---- timers ---------------------------------------------------------------

struct SleepAwaiter {
  std::chrono::nanoseconds d;
  bool await_ready() const noexcept { return d.count() <= 0; }
  void await_suspend(std::coroutine_handle<> h) {
    Reactor& r = current_reactor();
    r.add_timer(r.now() + d, h);
  }
  void await_resume() const noexcept {}
};
inline SleepAwaiter sleep_for(std::chrono::nanoseconds d) { return {d}; }

// with_timeout (design 02 §2.2): races t against a timer. On timeout the task keeps
// running detached until it finishes naturally, then its result is discarded (cooperative
// cancellation model, 02 §2.6). TODO(phase2): cancel the pending timer when t wins so
// reactor shutdown is not delayed by up to d.
template <class T>
Task<std::optional<T>> with_timeout(Task<T> t, std::chrono::nanoseconds d) {
  struct St {
    std::coroutine_handle<> waiter{};
    bool done = false;
    std::optional<T> val;
  };
  auto st = std::make_shared<St>();
  Reactor& r = current_reactor();

  spawn(
      [](std::shared_ptr<St> s, Task<T> inner) -> Task<void> {
        T v = co_await std::move(inner);
        s->done = true;
        if (s->waiter) {
          s->val.emplace(std::move(v));
          auto w = s->waiter;
          s->waiter = {};
          w.resume();
        }
      }(st, std::move(t)),
      r);
  spawn(
      [](std::shared_ptr<St> s, std::chrono::nanoseconds dd) -> Task<void> {
        co_await sleep_for(dd);
        if (!s->done && s->waiter) {
          auto w = s->waiter;
          s->waiter = {};
          w.resume();
        }
      }(st, d),
      r);

  struct Waiter {
    std::shared_ptr<St> st;
    bool await_ready() const noexcept { return st->done; }
    void await_suspend(std::coroutine_handle<> h) { st->waiter = h; }
    void await_resume() const noexcept {}
  };
  co_await Waiter{st};
  if (st->val.has_value()) co_return std::move(st->val);
  co_return std::nullopt;
}

}  // namespace lnfs::rt
