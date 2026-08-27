#pragma once
// Minimal metrics registry (design 08 §8.3): process-global counters exposed in
// Prometheus text format via the ctl socket and the optional metrics HTTP port.
// Component stats that live elsewhere (DRC, fd caches) register a text provider.
//
// Counters are sharded per thread slot with cache-line padding (plan doc 10 §2.6):
// every request used to do 2+ fetch_adds into one batch of shared cache lines; now
// each reactor thread bumps its own line and export sums the slots.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace lnfs::obs {

namespace detail {
inline size_t counter_slot() {
  static std::atomic<size_t> next{0};
  static thread_local size_t slot = next.fetch_add(1, std::memory_order_relaxed);
  return slot;
}
}  // namespace detail

// Drop-in for std::atomic<T> counters (fetch_add / fetch_sub / load): writes go to a
// per-thread slot on its own cache line, load() sums the slots (monotonic per slot, so
// relaxed sums are as consistent as the single atomic was).
template <class T>
class ShardedCounter {
 public:
  void fetch_add(T d, std::memory_order = std::memory_order_relaxed) {
    slots_[detail::counter_slot() & (kSlots - 1)].v.fetch_add(d,
                                                              std::memory_order_relaxed);
  }
  void fetch_sub(T d, std::memory_order = std::memory_order_relaxed) {
    slots_[detail::counter_slot() & (kSlots - 1)].v.fetch_sub(d,
                                                              std::memory_order_relaxed);
  }
  T load(std::memory_order = std::memory_order_relaxed) const {
    T sum{};
    for (const auto& s : slots_) sum += s.v.load(std::memory_order_relaxed);
    return sum;
  }

 private:
  static constexpr size_t kSlots = 8;  // power of two (slot index is masked)
  struct alignas(64) Slot {
    std::atomic<T> v{0};
  };
  Slot slots_[kSlots];
};

struct Metrics {
  // rpc / v3 engine (index = v3 procedure number, 0..21)
  static constexpr size_t kV3Procs = 22;
  ShardedCounter<uint64_t> v3_calls[kV3Procs]{};
  ShardedCounter<uint64_t> v3_errors[kV3Procs]{};
  ShardedCounter<uint64_t> v3_duration_us[kV3Procs]{};
  ShardedCounter<uint64_t> rpc_garbage{};
  ShardedCounter<uint64_t> mount_calls{};

  // transport
  ShardedCounter<uint64_t> conns_accepted{};
  ShardedCounter<int64_t> conns_active{};
  ShardedCounter<uint64_t> conns_rejected{};
  ShardedCounter<uint64_t> backpressure_waits{};

  // backend I/O through the v3 engine
  ShardedCounter<uint64_t> read_bytes{};
  ShardedCounter<uint64_t> write_bytes{};

  static Metrics& instance();
};

const char* v3_proc_name(uint32_t proc);

// Extra sections appended to the exposition (DRC, per-export fd caches, ...).
void register_text_provider(std::function<void(std::string&)> provider);
std::string prometheus_text();

}  // namespace lnfs::obs
