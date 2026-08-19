#include "runtime/buffer.hpp"

#include <cstdlib>
#include <new>

namespace lnfs::rt {

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
}  // namespace detail

BufferPool::BufferPool(Config cfg) : cfg_(cfg) {}

BufferPool::~BufferPool() {
  for (auto*& head : free_) {
    while (head) {
      auto* b = head;
      head = b->next_free;
      std::free(b->data);
      delete b;
    }
  }
}

static int class_for(size_t n) {
  if (n <= BufferPool::kSmall) return 0;
  if (n <= BufferPool::kMedium) return 1;
  if (n <= BufferPool::kLarge) return 2;
  return -1;
}
static size_t class_cap(int cls) {
  return cls == 0 ? BufferPool::kSmall : cls == 1 ? BufferPool::kMedium : BufferPool::kLarge;
}

detail::Block* BufferPool::take_or_create(int cls, size_t cap) {
  {
    std::lock_guard lk(mu_);
    if (cls >= 0 && free_[cls]) {
      auto* b = free_[cls];
      free_[cls] = b->next_free;
      free_bytes_ -= b->cap;
      b->refs.store(1, std::memory_order_relaxed);
      return b;
    }
  }
  auto* b = new detail::Block{static_cast<std::byte*>(std::malloc(cap)),
                              static_cast<uint32_t>(cap),
                              {1},
                              cls >= 0 ? this : nullptr,
                              nullptr};
  if (!b->data) throw std::bad_alloc();
  return b;
}

Buffer BufferPool::alloc(size_t n) {
  int cls = class_for(n);
  size_t cap = cls >= 0 ? class_cap(cls) : n;
  return Buffer(take_or_create(cls, cap));
}

void BufferPool::recycle(detail::Block* b) {
  int cls = class_for(b->cap);
  std::lock_guard lk(mu_);
  if (cls < 0 || free_bytes_ + b->cap > cfg_.max_free_bytes) {
    std::free(b->data);
    delete b;
    return;
  }
  b->next_free = free_[cls];
  free_[cls] = b;
  free_bytes_ += b->cap;
}

size_t BufferPool::free_bytes() {
  std::lock_guard lk(mu_);
  return free_bytes_;
}

}  // namespace lnfs::rt
