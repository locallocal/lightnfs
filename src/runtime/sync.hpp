#pragma once
// Coroutine synchronization primitives (design 02 §2.4). All are thread-safe: waiters are
// resumed by post()ing to the reactor they suspended on, so holders may live on any shard.
// Holding a lock across co_await is legal (that's the point).
//
// FIFO fairness throughout: ownership is handed directly to the oldest waiter on release.

#include <array>
#include <cassert>
#include <coroutine>
#include <cstdint>
#include <mutex>

#include "runtime/reactor.hpp"

namespace lnfs::rt {

namespace detail {
struct WaitNode {
  std::coroutine_handle<> h;
  Reactor* r = nullptr;
  WaitNode* next = nullptr;
  bool exclusive = true;  // used by AsyncSharedMutex
};

struct WaitList {  // intrusive FIFO
  WaitNode* head = nullptr;
  WaitNode* tail = nullptr;
  bool empty() const { return head == nullptr; }
  void push(WaitNode* n) {
    n->next = nullptr;
    if (tail) tail->next = n;
    else head = n;
    tail = n;
  }
  WaitNode* pop() {
    WaitNode* n = head;
    if (n) {
      head = n->next;
      if (!head) tail = nullptr;
    }
    return n;
  }
};

inline void resume_via_post(WaitNode* n) { n->r->post(n->h); }
}  // namespace detail

// ---- AsyncMutex -----------------------------------------------------------

class AsyncMutex {
 public:
  class Lock {
   public:
    Lock() = default;
    explicit Lock(AsyncMutex* m) : m_(m) {}
    Lock(Lock&& o) noexcept : m_(std::exchange(o.m_, nullptr)) {}
    Lock& operator=(Lock&& o) noexcept {
      if (this != &o) {
        reset();
        m_ = std::exchange(o.m_, nullptr);
      }
      return *this;
    }
    ~Lock() { reset(); }
    void reset() {
      if (m_) std::exchange(m_, nullptr)->unlock();
    }
    bool holds() const { return m_ != nullptr; }

   private:
    AsyncMutex* m_ = nullptr;
  };

  struct LockAwaiter {
    AsyncMutex& m;
    detail::WaitNode node{};
    bool await_ready() { return m.try_lock(); }
    bool await_suspend(std::coroutine_handle<> h) {
      std::lock_guard g(m.mu_);
      if (!m.locked_) {  // raced free between ready and suspend
        m.locked_ = true;
        return false;
      }
      node.h = h;
      node.r = &current_reactor();
      m.waiters_.push(&node);
      return true;
    }
    Lock await_resume() { return Lock(&m); }
  };

  [[nodiscard]] LockAwaiter lock() { return LockAwaiter{*this}; }
  bool try_lock() {
    std::lock_guard g(mu_);
    if (locked_) return false;
    locked_ = true;
    return true;
  }

 private:
  friend class Lock;
  void unlock() {
    detail::WaitNode* n = nullptr;
    {
      std::lock_guard g(mu_);
      assert(locked_);
      n = waiters_.pop();
      if (!n) locked_ = false;  // else: ownership transfers to n, stays locked
    }
    if (n) detail::resume_via_post(n);
  }

  std::mutex mu_;
  bool locked_ = false;
  detail::WaitList waiters_;
};

// ---- AsyncSharedMutex (per-object WCC sampling lock, design 02 §2.5) ------

class AsyncSharedMutex {
 public:
  class Lock {
   public:
    Lock() = default;
    Lock(AsyncSharedMutex* m, bool shared) : m_(m), shared_(shared) {}
    Lock(Lock&& o) noexcept : m_(std::exchange(o.m_, nullptr)), shared_(o.shared_) {}
    Lock& operator=(Lock&& o) noexcept {
      if (this != &o) {
        reset();
        m_ = std::exchange(o.m_, nullptr);
        shared_ = o.shared_;
      }
      return *this;
    }
    ~Lock() { reset(); }
    void reset() {
      if (!m_) return;
      auto* m = std::exchange(m_, nullptr);
      if (shared_) m->unlock_shared();
      else m->unlock();
    }

   private:
    AsyncSharedMutex* m_ = nullptr;
    bool shared_ = false;
  };
  using SharedLock = Lock;

  struct Awaiter {
    AsyncSharedMutex& m;
    bool shared;
    detail::WaitNode node{};
    bool await_ready() {
      std::lock_guard g(m.mu_);
      return m.try_grant_locked(shared);
    }
    bool await_suspend(std::coroutine_handle<> h) {
      std::lock_guard g(m.mu_);
      if (m.try_grant_locked(shared)) return false;
      node.h = h;
      node.r = &current_reactor();
      node.exclusive = !shared;
      m.waiters_.push(&node);
      return true;
    }
    Lock await_resume() { return Lock(&m, shared); }
  };

  [[nodiscard]] Awaiter lock() { return Awaiter{*this, false}; }
  [[nodiscard]] Awaiter lock_shared() { return Awaiter{*this, true}; }

 private:
  // FIFO: a request is granted only if compatible AND nothing older waits.
  bool try_grant_locked(bool shared) {
    if (!waiters_.empty()) return false;
    if (shared) {
      if (writer_) return false;
      ++readers_;
      return true;
    }
    if (writer_ || readers_ > 0) return false;
    writer_ = true;
    return true;
  }
  void grant_from_queue_locked(detail::WaitList& to_resume) {
    if (waiters_.empty() || writer_) return;
    if (waiters_.head->exclusive) {
      if (readers_ == 0) {
        writer_ = true;
        to_resume.push(waiters_.pop());
      }
      return;
    }
    while (!waiters_.empty() && !waiters_.head->exclusive) {
      ++readers_;
      to_resume.push(waiters_.pop());
    }
  }
  void unlock() {
    detail::WaitList ready;
    {
      std::lock_guard g(mu_);
      assert(writer_);
      writer_ = false;
      grant_from_queue_locked(ready);
    }
    while (auto* n = ready.pop()) detail::resume_via_post(n);
  }
  void unlock_shared() {
    detail::WaitList ready;
    {
      std::lock_guard g(mu_);
      assert(readers_ > 0);
      --readers_;
      grant_from_queue_locked(ready);
    }
    while (auto* n = ready.pop()) detail::resume_via_post(n);
  }

  std::mutex mu_;
  uint32_t readers_ = 0;
  bool writer_ = false;
  detail::WaitList waiters_;
};

// ---- AsyncCondVar ---------------------------------------------------------

class AsyncCondVar {
 public:
  // Atomically releases lk, waits for a notify, then re-acquires m into lk.
  Task<void> wait(AsyncMutex& m, AsyncMutex::Lock& lk) {
    struct Enqueue {
      AsyncCondVar& cv;
      AsyncMutex::Lock& lk;
      detail::WaitNode node{};
      bool await_ready() const noexcept { return false; }
      void await_suspend(std::coroutine_handle<> h) {
        node.h = h;
        node.r = &current_reactor();
        {
          std::lock_guard g(cv.mu_);
          cv.waiters_.push(&node);
        }
        lk.reset();  // release only after we are queued: no lost wakeups
      }
      void await_resume() const noexcept {}
    };
    co_await Enqueue{*this, lk};
    lk = co_await m.lock();
  }

  void notify_one() {
    detail::WaitNode* n;
    {
      std::lock_guard g(mu_);
      n = waiters_.pop();
    }
    if (n) detail::resume_via_post(n);
  }
  void notify_all() {
    detail::WaitList ready;
    {
      std::lock_guard g(mu_);
      while (auto* n = waiters_.pop()) ready.push(n);
    }
    while (auto* n = ready.pop()) detail::resume_via_post(n);
  }

 private:
  std::mutex mu_;
  detail::WaitList waiters_;
};

// ---- Semaphore (offload capacity, per-conn inflight; design 02 §2.4) ------

class Semaphore {
 public:
  explicit Semaphore(int64_t count) : count_(count) {}

  struct Awaiter {
    Semaphore& s;
    detail::WaitNode node{};
    bool await_ready() {
      std::lock_guard g(s.mu_);
      if (s.count_ > 0) {
        --s.count_;
        return true;
      }
      return false;
    }
    bool await_suspend(std::coroutine_handle<> h) {
      std::lock_guard g(s.mu_);
      if (s.count_ > 0) {
        --s.count_;
        return false;
      }
      node.h = h;
      node.r = &current_reactor();
      s.waiters_.push(&node);
      return true;
    }
    void await_resume() const noexcept {}
  };

  [[nodiscard]] Awaiter acquire() { return Awaiter{*this}; }
  void release() {
    detail::WaitNode* n;
    {
      std::lock_guard g(mu_);
      n = waiters_.pop();
      if (!n) ++count_;  // else: the permit transfers to n
    }
    if (n) detail::resume_via_post(n);
  }
  int64_t available() {
    std::lock_guard g(mu_);
    return count_;
  }

 private:
  std::mutex mu_;
  int64_t count_;
  detail::WaitList waiters_;
};

// ---- Event: one-shot, multi-waiter ----------------------------------------

class Event {
 public:
  struct Awaiter {
    Event& e;
    detail::WaitNode node{};
    bool await_ready() {
      std::lock_guard g(e.mu_);
      return e.set_;
    }
    bool await_suspend(std::coroutine_handle<> h) {
      std::lock_guard g(e.mu_);
      if (e.set_) return false;
      node.h = h;
      node.r = &current_reactor();
      e.waiters_.push(&node);
      return true;
    }
    void await_resume() const noexcept {}
  };
  [[nodiscard]] Awaiter wait() { return Awaiter{*this}; }
  void set() {
    detail::WaitList ready;
    {
      std::lock_guard g(mu_);
      set_ = true;
      while (auto* n = waiters_.pop()) ready.push(n);
    }
    while (auto* n = ready.pop()) detail::resume_via_post(n);
  }
  bool is_set() {
    std::lock_guard g(mu_);
    return set_;
  }
  void reset() {
    std::lock_guard g(mu_);
    set_ = false;
  }

 private:
  std::mutex mu_;
  bool set_ = false;
  detail::WaitList waiters_;
};

// ---- Sharded<T, N> (design 02 §2.4: every global table is sharded) --------

template <class T, size_t N = 16>
class Sharded {
 public:
  static_assert(N > 0);
  struct Shard {
    AsyncMutex mu;
    T v{};
  };
  Shard& shard(uint64_t hash) { return shards_[hash % N]; }
  static constexpr size_t shard_count() { return N; }
  Shard& shard_at(size_t i) { return shards_[i]; }

 private:
  std::array<Shard, N> shards_;
};

}  // namespace lnfs::rt
