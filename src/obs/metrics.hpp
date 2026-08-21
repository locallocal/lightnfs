#pragma once
// Minimal metrics registry (design 08 §8.3): process-global atomic counters exposed in
// Prometheus text format via the ctl socket and the optional metrics HTTP port.
// Component stats that live elsewhere (DRC, fd caches) register a text provider.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace lnfs::obs {

struct Metrics {
  // rpc / v3 engine (index = v3 procedure number, 0..21)
  static constexpr size_t kV3Procs = 22;
  std::atomic<uint64_t> v3_calls[kV3Procs]{};
  std::atomic<uint64_t> v3_errors[kV3Procs]{};
  std::atomic<uint64_t> v3_duration_us[kV3Procs]{};
  std::atomic<uint64_t> rpc_garbage{0};
  std::atomic<uint64_t> mount_calls{0};

  // transport
  std::atomic<uint64_t> conns_accepted{0};
  std::atomic<int64_t> conns_active{0};
  std::atomic<uint64_t> conns_rejected{0};
  std::atomic<uint64_t> backpressure_waits{0};

  // backend I/O through the v3 engine
  std::atomic<uint64_t> read_bytes{0};
  std::atomic<uint64_t> write_bytes{0};

  static Metrics& instance();
};

const char* v3_proc_name(uint32_t proc);

// Extra sections appended to the exposition (DRC, per-export fd caches, ...).
void register_text_provider(std::function<void(std::string&)> provider);
std::string prometheus_text();

}  // namespace lnfs::obs
