#pragma once
// Minimal metrics registry (design 08 §8.3): process-global counters exposed in
// Prometheus text format via the ctl socket and the optional metrics HTTP port.
// Component stats that live elsewhere (DRC, fd caches) register a text provider.
//
// Counters are sharded per thread slot with cache-line padding (plan doc 10 §2.6):
// every request used to do 2+ fetch_adds into one batch of shared cache lines; now
// each reactor thread bumps its own line and export sums the slots.
//
// Latency histograms (plan doc 10 §3.2) use fixed buckets with Prometheus histogram
// semantics so p99-style SLIs can be computed from the exposition; observations go to
// per-thread slots like the counters do.

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
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

// Fixed-bucket latency histogram (Prometheus histogram semantics). Buckets cover
// 100µs..5s — NFS request latencies below the first bound are healthy and need no
// resolution, above the last bound the slow-request log names the culprit anyway.
class LatencyHistogram {
 public:
  static constexpr uint64_t kBoundsUs[] = {100,    250,    500,     1000,    2500,
                                           5000,   10000,  25000,   50000,   100000,
                                           250000, 500000, 1000000, 2500000, 5000000};
  static constexpr size_t kBuckets = std::size(kBoundsUs) + 1;  // + the +Inf bucket

  void observe_us(uint64_t us) {
    size_t b = 0;
    while (b < kBuckets - 1 && us > kBoundsUs[b]) ++b;
    auto& s = slots_[detail::counter_slot() & (kSlots - 1)];
    s.buckets[b].fetch_add(1, std::memory_order_relaxed);
    s.sum_us.fetch_add(us, std::memory_order_relaxed);
  }

  struct Snapshot {
    uint64_t buckets[kBuckets]{};  // per-bucket (non-cumulative) counts
    uint64_t sum_us = 0;
    uint64_t count = 0;
  };
  Snapshot snapshot() const {
    Snapshot out;
    for (const auto& s : slots_) {
      for (size_t b = 0; b < kBuckets; ++b) {
        uint64_t v = s.buckets[b].load(std::memory_order_relaxed);
        out.buckets[b] += v;
        out.count += v;
      }
      out.sum_us += s.sum_us.load(std::memory_order_relaxed);
    }
    return out;
  }

 private:
  static constexpr size_t kSlots = 4;  // power of two (slot index is masked)
  struct alignas(64) Slot {
    std::atomic<uint64_t> buckets[kBuckets]{};
    std::atomic<uint64_t> sum_us{0};
  };
  Slot slots_[kSlots];
};

// Per-export data-path counters (plan doc 10 §3.3): owned by the ExportEntry and
// exported with {export, fsid} labels so multi-export deployments can localize load.
struct ExportMetrics {
  ShardedCounter<uint64_t> read_bytes{}, write_bytes{};
  ShardedCounter<uint64_t> read_ops{}, write_ops{};
};

struct Metrics {
  // rpc / v3 engine (index = v3 procedure number, 0..21)
  static constexpr size_t kV3Procs = 22;
  ShardedCounter<uint64_t> v3_calls[kV3Procs]{};
  ShardedCounter<uint64_t> v3_errors[kV3Procs]{};
  ShardedCounter<uint64_t> v3_duration_us[kV3Procs]{};
  LatencyHistogram v3_duration[kV3Procs]{};
  ShardedCounter<uint64_t> rpc_garbage{};
  ShardedCounter<uint64_t> mount_calls{};

  // v4 engine (plan doc 10 §3.1): per-op calls/errors/latency indexed by opcode
  // (3..71 = nfsv4 kLastKnownOp; slot 0 collects out-of-table opcodes) plus a
  // whole-COMPOUND latency histogram (count == compounds served).
  static constexpr size_t kV4Ops = 72;
  ShardedCounter<uint64_t> v4_op_calls[kV4Ops]{};
  ShardedCounter<uint64_t> v4_op_errors[kV4Ops]{};
  LatencyHistogram v4_op_duration[kV4Ops]{};
  // Label for the exposition, stored at bump time (always a static string literal);
  // obs cannot depend on the nfsv4 op table.
  std::atomic<const char*> v4_op_names[kV4Ops]{};
  LatencyHistogram v4_compound_duration{};

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
inline size_t v4_op_index(uint32_t opcode) {
  return opcode < Metrics::kV4Ops ? opcode : 0;
}

// Slow-request threshold (plan doc 10 §3.6, design 08 §8.4): requests slower than this
// log a warn line with a per-op time breakdown. 0 disables.
void set_slow_request_threshold_us(uint64_t us);
uint64_t slow_request_threshold_us();

// Appends one Prometheus histogram series ({name}_bucket/_sum/_count) with the given
// label set ("" for none). Bounds are microseconds; the exposition uses seconds.
void append_histogram(std::string& out, std::string_view name, std::string_view labels,
                      const LatencyHistogram::Snapshot& snap);
void append_histogram(std::string& out, std::string_view name, std::string_view labels,
                      std::span<const uint64_t> bounds_us,
                      std::span<const uint64_t> buckets, uint64_t sum_us);

// Extra sections appended to the exposition (DRC, per-export fd caches, ...).
void register_text_provider(std::function<void(std::string&)> provider);
std::string prometheus_text();

}  // namespace lnfs::obs
