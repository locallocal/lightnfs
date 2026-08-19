#pragma once
// Reference-counted buffers and chains (design 02 §2.7). Receive path: RecordStream slices
// recv buffers into BufferChains without copying; XDR decodes on the chain. Send path: reply
// headers in small buffers, READ data in its own buffer, stitched by writev.
// Pool: size classes 4K/64K/1M, freelists, byte watermark.

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
    size_t max_free_bytes = 64 << 20;  // watermark of cached free memory
  };
  BufferPool() : BufferPool(Config{}) {}
  explicit BufferPool(Config cfg);
  ~BufferPool();

  static constexpr size_t kSmall = 4 * 1024;
  static constexpr size_t kMedium = 64 * 1024;
  static constexpr size_t kLarge = 1024 * 1024;

  Buffer alloc(size_t n);  // rounds up to a class; > kLarge gets an exact unpooled block
  size_t free_bytes();

 private:
  friend void detail::block_unref(detail::Block*);
  void recycle(detail::Block* b);
  detail::Block* take_or_create(int cls, size_t cap);

  std::mutex mu_;
  detail::Block* free_[3] = {nullptr, nullptr, nullptr};
  size_t free_bytes_ = 0;
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
  void to_iovecs(std::vector<iovec>& out, size_t skip = 0) const {
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
