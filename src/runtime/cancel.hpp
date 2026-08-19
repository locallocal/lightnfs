#pragma once
// Cooperative cancellation (design 02 §2.6): per-request contexts carry a CancelToken set
// on connection teardown; long chains check it at natural yield points. No forced coroutine
// destruction — in-flight ring ops are cancelled best-effort via uring_cancel_fd.

#include <atomic>
#include <memory>

namespace lnfs::rt {

namespace detail {
struct CancelState {
  std::atomic<bool> requested{false};
};
}  // namespace detail

class CancelToken {
 public:
  CancelToken() = default;
  bool cancel_requested() const {
    return s_ && s_->requested.load(std::memory_order_relaxed);
  }

 private:
  friend class CancelSource;
  explicit CancelToken(std::shared_ptr<const detail::CancelState> s) : s_(std::move(s)) {}
  std::shared_ptr<const detail::CancelState> s_;
};

class CancelSource {
 public:
  CancelSource() : s_(std::make_shared<detail::CancelState>()) {}
  void request() { s_->requested.store(true, std::memory_order_relaxed); }
  bool cancel_requested() const { return s_->requested.load(std::memory_order_relaxed); }
  CancelToken token() const { return CancelToken(s_); }

 private:
  std::shared_ptr<detail::CancelState> s_;
};

}  // namespace lnfs::rt
