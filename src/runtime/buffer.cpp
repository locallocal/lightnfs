#include "runtime/buffer.hpp"

#include <algorithm>
#include <cstdlib>
#include <new>

namespace lnfs::rt {

static_assert(BufferPool::kClasses == 5 &&
              sizeof(detail::Magazine::head) / sizeof(detail::Block*) == 5);

namespace detail {

void block_unref(Block* b) {
  if (b->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
  if (b->pool) {
    b->pool->recycle(b);
  } else {
    std::free(b->data);
    delete b;
  }
}

namespace {

void free_block(Block* b) {
  std::free(b->data);
  delete b;
}

// Links every live pool<->magazine pair. Taken only on the cold paths (first alloc of a
// pool on a thread, thread exit, pool destruction); the hot path takes only mag->mu.
// Deliberately leaked: thread_local destructors on the main thread can outlive
// function-local statics.
std::mutex& registry_mu() {
  static std::mutex* mu = new std::mutex;
  return *mu;
}

// Owns this thread's magazines; the destructor flushes surviving blocks back to their
// pools (or frees them when the pool died first).
struct ThreadMagazines {
  std::vector<std::unique_ptr<Magazine>> mags;
  ~ThreadMagazines();
};
thread_local ThreadMagazines t_mags;

}  // namespace
}  // namespace detail

BufferPool::BufferPool(Config cfg) : cfg_(cfg) {}

BufferPool::~BufferPool() {
  // Detach magazines first so no thread recycles into this pool afterwards; their
  // cached blocks die here. The registry mutex is held across the whole sweep so a
  // concurrently exiting thread cannot delete a magazine out from under this loop.
  {
    std::lock_guard reg(detail::registry_mu());
    for (auto* m : magazines_) {
      std::lock_guard lk(m->mu);
      m->pool = nullptr;
      for (int c = 0; c < kClasses; ++c) {
        while (auto* b = m->head[c]) {
          m->head[c] = b->next_free;
          detail::free_block(b);
        }
        m->count[c] = 0;
      }
    }
    magazines_.clear();
  }
  std::lock_guard lk(mu_);
  drop_free_locked();
}

void detail::magazine_thread_exit(detail::Magazine* m) {
  std::lock_guard reg(detail::registry_mu());
  std::lock_guard lk(m->mu);
  if (m->pool) {
    auto& reg_list = m->pool->magazines_;
    reg_list.erase(std::remove(reg_list.begin(), reg_list.end(), m), reg_list.end());
  }
  for (int c = 0; c < BufferPool::kClasses; ++c) {
    while (auto* b = m->head[c]) {
      m->head[c] = b->next_free;
      if (m->pool) m->pool->global_put(b, c);
      else detail::free_block(b);
    }
  }
}

detail::ThreadMagazines::~ThreadMagazines() {
  for (auto& m : mags) detail::magazine_thread_exit(m.get());
}

detail::Magazine* BufferPool::magazine() {
  for (auto& m : detail::t_mags.mags)
    if (m->pool == this) return m.get();
  auto mag = std::make_unique<detail::Magazine>(this);
  detail::Magazine* raw = mag.get();
  {
    std::lock_guard reg(detail::registry_mu());
    magazines_.push_back(raw);
  }
  detail::t_mags.mags.push_back(std::move(mag));
  return raw;
}

static int class_for(size_t n) {
  for (int c = 0; c < BufferPool::kClasses; ++c)
    if (n <= BufferPool::kClassCaps[c]) return c;
  return -1;
}

detail::Block* BufferPool::global_take(int cls) {
  std::lock_guard lk(mu_);
  auto* b = free_[cls];
  if (b) {
    free_[cls] = b->next_free;
    free_bytes_ -= b->cap;
  }
  return b;
}

void BufferPool::global_put(detail::Block* b, int cls) {
  std::lock_guard lk(mu_);
  if (free_bytes_ + b->cap > cfg_.max_free_bytes) {
    detail::free_block(b);
    return;
  }
  b->next_free = free_[cls];
  free_[cls] = b;
  free_bytes_ += b->cap;
}

void BufferPool::drop_free_locked() {
  for (auto*& head : free_) {
    while (head) {
      auto* b = head;
      head = b->next_free;
      detail::free_block(b);
    }
  }
  free_bytes_ = 0;
}

detail::Block* BufferPool::take_or_create(int cls, size_t cap) {
  if (cls >= 0) {
    detail::Magazine* mag = magazine();
    {
      std::lock_guard lk(mag->mu);
      if (mag->pool == this && mag->head[cls]) {
        auto* b = mag->head[cls];
        mag->head[cls] = b->next_free;
        --mag->count[cls];
        b->refs.store(1, std::memory_order_relaxed);
        return b;
      }
    }
    if (auto* b = global_take(cls)) {
      b->refs.store(1, std::memory_order_relaxed);
      return b;
    }
  }
  std::byte* mem = static_cast<std::byte*>(std::malloc(cap));
  if (!mem) {
    // OOM degradation (plan doc 10 §2.4): give back every cached byte and retry once
    // before surfacing the failure to the caller.
    {
      std::lock_guard lk(mu_);
      drop_free_locked();
    }
    mem = static_cast<std::byte*>(std::malloc(cap));
    if (!mem) throw std::bad_alloc();
  }
  return new detail::Block{mem, static_cast<uint32_t>(cap), {1}, cls >= 0 ? this : nullptr,
                           nullptr};
}

Buffer BufferPool::alloc(size_t n) {
  int cls = class_for(n);
  size_t cap = cls >= 0 ? kClassCaps[cls] : n;
  return Buffer(take_or_create(cls, cap));
}

void BufferPool::recycle(detail::Block* b) {
  int cls = class_for(b->cap);
  if (cls < 0 || kClassCaps[cls] != b->cap) {  // odd size (config change): don't cache
    detail::free_block(b);
    return;
  }
  // Freeing thread's magazine first; overflow spills to the global freelist. Blocks may
  // migrate between threads' magazines — harmless, the memory is process-global.
  detail::Magazine* mag = magazine();
  {
    std::lock_guard lk(mag->mu);
    if (mag->pool == this && mag->count[cls] < kMagazineCap) {
      b->next_free = mag->head[cls];
      mag->head[cls] = b;
      ++mag->count[cls];
      return;
    }
  }
  global_put(b, cls);
}

size_t BufferPool::free_bytes() {
  std::lock_guard lk(mu_);
  return free_bytes_;
}

}  // namespace lnfs::rt
