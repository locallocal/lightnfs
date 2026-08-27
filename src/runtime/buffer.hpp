#pragma once
// Reference-counted buffers and chains (design 02 §2.7). Receive path: RecordStream slices
// recv buffers into BufferChains without copying; XDR decodes on the chain. Send path: reply
// headers in small buffers, READ data in its own buffer, stitched by writev.
// Pool: size classes 4K/64K/128K/256K/1M, per-thread magazines over a locked global
// freelist, byte watermark (plan doc 10 §2.4: almost all alloc/free pairs are same-reactor,
// so the hot path touches only a thread-local, uncontended magazine).

#include <sys/uio.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "util/small_vec.hpp"

namespace lnfs::rt {

class BufferPool;

namespace detail {
struct Block {
  std::byte* data;
  uint32_t cap;
  std::atomic<uint32_t> refs;
  BufferPool* pool;  // nullptr: plain malloc'd oversize block
  Block* next_free;
};
void block_unref(Block* b);

// Per-(thread, pool) block cache. The owner thread touches it under mag mutex only
// (uncontended, stays in its own cache lines); the pool destructor and the thread-exit
// path detach it under a global registry mutex (rare). Lock order: registry -> mag.
struct Magazine {
  std::mutex mu;
  BufferPool* pool;  // null once the pool died; blocks were reclaimed by the pool
  Block* head[5] = {};
  uint32_t count[5] = {};
  explicit Magazine(BufferPool* p) : pool(p) {}
};
// Thread-exit path: unregister m from its pool and flush its cached blocks back.
void magazine_thread_exit(Magazine* m);
}  // namespace detail

class Buffer {
 public:
  Buffer() = default;
  Buffer(const Buffer& o) : b_(o.b_) {
    if (b_) b_->refs.fetch_add(1, std::memory_order_relaxed);
  }
  Buffer(Buffer&& o) noexcept : b_(std::exchange(o.b_, nullptr)) {}
  Buffer& operator=(Buffer o) noexcept {
    std::swap(b_, o.b_);
    return *this;
  }
  ~Buffer() {
    if (b_) detail::block_unref(b_);
  }

  std::byte* data() const { return b_ ? b_->data : nullptr; }
  size_t capacity() const { return b_ ? b_->cap : 0; }
  explicit operator bool() const { return b_ != nullptr; }

 private:
  friend class BufferPool;
  explicit Buffer(detail::Block* b) : b_(b) {}
  detail::Block* b_ = nullptr;
};

class BufferPool {
 public:
  struct Config {
    size_t max_free_bytes = 64 << 20;  // watermark of cached free memory (global freelist)
  };
  BufferPool() : BufferPool(Config{}) {}
  explicit BufferPool(Config cfg);
  ~BufferPool();

  static constexpr size_t kSmall = 4 * 1024;
  static constexpr size_t kMedium = 64 * 1024;
  static constexpr size_t kLarge = 1024 * 1024;
  // 128K/256K sit between the typical recv buffer and the READ ceiling: common
  // rsize/wsize allocations used to round all the way up to the 1M class (§2.4).
  static constexpr size_t kClassCaps[] = {4 * 1024, 64 * 1024, 128 * 1024, 256 * 1024,
                                          1024 * 1024};
  static constexpr int kClasses = 5;
  // Per-thread magazine depth per class, sized so a request burst stays thread-local
  // without hoarding: 16 × 1M worst case = 16MB per active thread.
  static constexpr uint32_t kMagazineCap = 16;

  Buffer alloc(size_t n);  // rounds up to a class; > kLarge gets an exact unpooled block
  size_t free_bytes();

 private:
  friend void detail::block_unref(detail::Block*);
  friend void detail::magazine_thread_exit(detail::Magazine*);
  void recycle(detail::Block* b);
  detail::Block* take_or_create(int cls, size_t cap);
  detail::Block* global_take(int cls);            // locked freelist pop (nullptr on empty)
  void global_put(detail::Block* b, int cls);     // locked freelist push / free over watermark
  void drop_free_locked();                        // caller holds mu_: free the whole freelist
  detail::Magazine* magazine();                   // this thread's magazine for this pool

  std::mutex mu_;
  detail::Block* free_[kClasses] = {};
  size_t free_bytes_ = 0;
  std::vector<detail::Magazine*> magazines_;  // guarded by the global registry mutex
  Config cfg_;
};

// A byte sequence assembled from buffer slices. Used for both received records and replies.
class BufferChain {
 public:
  struct Seg {
    Buffer buf;
    uint32_t off;
    uint32_t len;
  };

  void append(Buffer b, uint32_t off, uint32_t len) {
    if (len == 0) return;
    size_ += len;
    segs_.emplace_back(Seg{std::move(b), off, len});
  }
  void append_chain(BufferChain&& o) {
    for (auto& s : o.segs_) {
      size_ += s.len;
      segs_.emplace_back(std::move(s));
    }
    o.segs_.clear();
    o.size_ = 0;
  }
  size_t size() const { return size_; }
  size_t seg_count() const { return segs_.size(); }
  const Seg& seg(size_t i) const { return segs_[i]; }
  Seg& seg(size_t i) { return segs_[i]; }
  bool empty() const { return size_ == 0; }
  // Removes and returns the last segment (XdrEnc rollback support).
  Seg pop_back() {
    Seg s = std::move(segs_[segs_.size() - 1]);
    segs_.pop_back();
    size_ -= s.len;
    return s;
  }
  void clear() {
    segs_.clear();
    size_ = 0;
  }

  // Flatten (tests / small headers).
  std::vector<std::byte> to_bytes() const {
    std::vector<std::byte> out;
    out.reserve(size_);
    for (const auto& s : segs_)
      out.insert(out.end(), s.buf.data() + s.off, s.buf.data() + s.off + s.len);
    return out;
  }

  // Build iovecs skipping the first `skip` bytes (partial-send continuation).
  // Vec: any container with clear()/push_back(iovec) (std::vector, SmallVec).
  template <class Vec>
  void to_iovecs(Vec& out, size_t skip = 0) const {
    out.clear();
    for (const auto& s : segs_) {
      if (skip >= s.len) {
        skip -= s.len;
        continue;
      }
      out.push_back(iovec{s.buf.data() + s.off + skip, s.len - skip});
      skip = 0;
    }
  }

 private:
  SmallVec<Seg, 4> segs_;
  size_t size_ = 0;
};

using SendBuf = BufferChain;

}  // namespace lnfs::rt
