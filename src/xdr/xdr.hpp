#pragma once
// XDR encode/decode over buffer chains (design 03 §3.6).
//  - XdrDec walks a received BufferChain; opaque()/string() return zero-copy views into the
//    chain when the field is contiguous (spanning fields are gathered into decoder-owned
//    scratch). Any bounds/max violation returns Err(kGarbage) — the caller maps that to
//    GARBAGE_ARGS at the RPC boundary.
//  - XdrEnc appends into pooled buffers; raw_gap() reserves patch-later space (record marks,
//    COMPOUND status), attach() splices a data buffer in without copying (READ path).

#include <bit>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include "runtime/buffer.hpp"
#include "util/result.hpp"

namespace lnfs::xdr {

inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }
inline uint32_t to_be32(uint32_t v) {
  if constexpr (std::endian::native == std::endian::little) return bswap32(v);
  return v;
}
inline uint64_t to_be64(uint64_t v) {
  if constexpr (std::endian::native == std::endian::little) return bswap64(v);
  return v;
}
inline uint32_t from_be32(uint32_t v) { return to_be32(v); }
inline uint64_t from_be64(uint64_t v) { return to_be64(v); }
constexpr uint32_t pad4(uint32_t n) { return (4 - (n & 3)) & 3; }

class XdrDec {
 public:
  explicit XdrDec(const rt::BufferChain& chain) : chain_(&chain), remaining_(chain.size()) {}
  // Flat-span mode: decode from contiguous memory the caller keeps alive (auth bodies etc.).
  explicit XdrDec(std::span<const std::byte> flat) : flat_(flat), remaining_(flat.size()) {}
  XdrDec(XdrDec&&) = default;
  XdrDec& operator=(XdrDec&&) = default;

  size_t remaining() const { return remaining_; }
  bool at_end() const { return remaining_ == 0; }

  Result<uint32_t> u32() {
    const std::byte* p = LNFS_TRY(take(4));
    uint32_t v;
    std::memcpy(&v, p, 4);
    return from_be32(v);
  }
  Result<uint64_t> u64() {
    const std::byte* p = LNFS_TRY(take(8));
    uint64_t v;
    std::memcpy(&v, p, 8);
    return from_be64(v);
  }
  Result<bool> boolean() {
    uint32_t v = LNFS_TRY(u32());
    if (v > 1) return Err(Errno::kGarbage);
    return v == 1;
  }
  // Fixed-length opaque (caller supplies n); consumes padding.
  Result<std::span<const std::byte>> opaque_fixed(uint32_t n) {
    const std::byte* p = LNFS_TRY(take(n));
    LNFS_TRY(skip(pad4(n)));
    return std::span<const std::byte>(p, n);
  }
  // Variable-length opaque<max>.
  Result<std::span<const std::byte>> opaque(uint32_t max) {
    uint32_t len = LNFS_TRY(u32());
    if (len > max) return Err(Errno::kGarbage);
    return opaque_fixed(len);
  }
  Result<std::string_view> string(uint32_t max) {
    auto s = LNFS_TRY(opaque(max));
    return std::string_view(reinterpret_cast<const char*>(s.data()), s.size());
  }
  Result<void> skip(size_t n) {
    if (n == 0) return {};
    LNFS_TRY(take(n));
    return {};
  }

 private:
  // A contiguous view of the next n bytes; gathers across segments into scratch when needed.
  Result<const std::byte*> take(size_t n) {
    if (n > remaining_) return Err(Errno::kGarbage);
    if (n == 0) return static_cast<const std::byte*>(nullptr);
    if (!chain_) {  // flat mode
      const std::byte* p = flat_.data() + off_;
      off_ += n;
      remaining_ -= n;
      return p;
    }
    // skip empty/finished segments
    while (seg_ < chain_->seg_count() && off_ == chain_->seg(seg_).len) {
      ++seg_;
      off_ = 0;
    }
    const auto& s = chain_->seg(seg_);
    if (s.len - off_ >= n) {
      const std::byte* p = s.buf.data() + s.off + off_;
      off_ += n;
      remaining_ -= n;
      return p;
    }
    // spanning: gather
    auto owned = std::make_unique<std::byte[]>(n);
    std::byte* dst = owned.get();
    size_t need = n;
    while (need > 0) {
      const auto& cs = chain_->seg(seg_);
      size_t avail = cs.len - off_;
      if (avail == 0) {
        ++seg_;
        off_ = 0;
        continue;
      }
      size_t k = std::min(avail, need);
      std::memcpy(dst, cs.buf.data() + cs.off + off_, k);
      dst += k;
      off_ += k;
      need -= k;
    }
    remaining_ -= n;
    scratch_.push_back(std::move(owned));
    return scratch_.back().get();
  }

  const rt::BufferChain* chain_ = nullptr;
  std::span<const std::byte> flat_{};
  size_t seg_ = 0;
  size_t off_ = 0;
  size_t remaining_;
  std::vector<std::unique_ptr<std::byte[]>> scratch_;
};

class XdrEnc {
 public:
  explicit XdrEnc(rt::BufferPool& pool, size_t limit = SIZE_MAX) : pool_(pool), limit_(limit) {}

  size_t size() const { return size_; }
  bool overflowed() const { return overflow_; }

  void u32(uint32_t v) {
    v = to_be32(v);
    put(&v, 4);
  }
  void u64(uint64_t v) {
    v = to_be64(v);
    put(&v, 8);
  }
  void boolean(bool b) { u32(b ? 1 : 0); }
  void opaque_fixed(std::span<const std::byte> s) {
    put(s.data(), s.size());
    static const char zeros[4] = {};
    put(zeros, pad4(static_cast<uint32_t>(s.size())));
  }
  void opaque(std::span<const std::byte> s) {
    u32(static_cast<uint32_t>(s.size()));
    opaque_fixed(s);
  }
  void string(std::string_view s) {
    opaque(std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()));
  }

  // Reserve n contiguous bytes to be patched later (e.g. COMPOUND status). The pointer stays
  // valid for the lifetime of the produced SendBuf (buffer memory is refcounted and fixed).
  std::byte* raw_gap(size_t n) {
    ensure(n);
    std::byte* p = tail_.data() + tail_used_;
    tail_used_ += n;
    size_ += n;
    check_limit();
    return p;
  }
  // Zero-copy splice of an externally filled buffer (READ data). Caller encodes the XDR
  // length itself (u32) before attaching; padding is added here.
  void attach(rt::Buffer data, uint32_t off, uint32_t len) {
    close_tail();
    out_.append(std::move(data), off, len);
    size_ += len;
    static const char zeros[4] = {};
    put(zeros, pad4(len));
    check_limit();
  }

  rt::SendBuf take() {
    close_tail();
    rt::SendBuf r = std::move(out_);
    out_.clear();
    size_ = 0;
    return r;
  }

 private:
  void put(const void* p, size_t n) {
    if (n == 0) return;
    ensure(n);
    std::memcpy(tail_.data() + tail_used_, p, n);
    tail_used_ += n;
    size_ += n;
    check_limit();
  }
  void ensure(size_t n) {
    if (tail_ && tail_used_ + n <= tail_.capacity()) return;
    close_tail();
    tail_ = pool_.alloc(std::max<size_t>(n, rt::BufferPool::kSmall));
    tail_used_ = 0;
  }
  void close_tail() {
    if (tail_ && tail_used_ > 0)
      out_.append(std::move(tail_), 0, static_cast<uint32_t>(tail_used_));
    tail_ = rt::Buffer();
    tail_used_ = 0;
  }
  void check_limit() {
    if (size_ > limit_) overflow_ = true;
  }

  rt::BufferPool& pool_;
  rt::SendBuf out_;
  rt::Buffer tail_;
  size_t tail_used_ = 0;
  size_t size_ = 0;
  size_t limit_;
  bool overflow_ = false;
};

}  // namespace lnfs::xdr
