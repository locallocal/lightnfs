#pragma once
// Pooled coroutine-frame allocation (plan doc 10 §2.4). A single GETATTR runs a chain of
// 7-8 coroutine frames; without this every frame is a global operator new/delete pair.
// Thread-local size-class freelists recycle frames instead. Frames may be freed on a
// different thread than they were allocated on (main-thread spawn, offload edges) — that
// only migrates a slot between thread caches, the memory itself is process-global.

#include <cstddef>
#include <cstdlib>
#include <new>

namespace lnfs::rt::detail {

// 64-byte granularity up to 4KB: 64 bins. Larger frames (rare: big local state) fall
// through to the global heap.
inline constexpr size_t kFrameGranule = 64;
inline constexpr size_t kFrameMaxPooled = 4096;
inline constexpr size_t kFrameBins = kFrameMaxPooled / kFrameGranule;
inline constexpr size_t kFrameBinCap = 64;  // slots cached per bin per thread

struct FrameBins {
  void* head[kFrameBins] = {};
  size_t count[kFrameBins] = {};
  ~FrameBins() {
    for (auto* h : head) {
      while (h) {
        void* next = *static_cast<void**>(h);
        std::free(h);
        h = next;
      }
    }
  }
};
inline thread_local FrameBins t_frame_bins;

inline void* frame_alloc(size_t n) {
  if (n == 0) n = 1;
  size_t idx = (n - 1) / kFrameGranule;
  if (idx >= kFrameBins) {
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
  }
  FrameBins& bins = t_frame_bins;
  if (void* p = bins.head[idx]) {
    bins.head[idx] = *static_cast<void**>(p);
    --bins.count[idx];
    return p;
  }
  void* p = std::malloc((idx + 1) * kFrameGranule);
  if (!p) throw std::bad_alloc();
  return p;
}

inline void frame_free(void* p, size_t n) noexcept {
  if (n == 0) n = 1;
  size_t idx = (n - 1) / kFrameGranule;
  if (idx >= kFrameBins) {
    std::free(p);
    return;
  }
  FrameBins& bins = t_frame_bins;
  if (bins.count[idx] >= kFrameBinCap) {
    std::free(p);
    return;
  }
  *static_cast<void**>(p) = bins.head[idx];
  bins.head[idx] = p;
  ++bins.count[idx];
}

}  // namespace lnfs::rt::detail
