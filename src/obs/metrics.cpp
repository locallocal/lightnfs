#include "obs/metrics.hpp"

#include <format>
#include <mutex>
#include <vector>

namespace lnfs::obs {

Metrics& Metrics::instance() {
  static Metrics m;
  return m;
}

const char* v3_proc_name(uint32_t proc) {
  static const char* names[] = {
      "NULL",   "GETATTR", "SETATTR", "LOOKUP", "ACCESS",  "READLINK",
      "READ",   "WRITE",   "CREATE",  "MKDIR",  "SYMLINK", "MKNOD",
      "REMOVE", "RMDIR",   "RENAME",  "LINK",   "READDIR", "READDIRPLUS",
      "FSSTAT", "FSINFO",  "PATHCONF", "COMMIT"};
  return proc < 22 ? names[proc] : "?";
}

namespace {
std::mutex g_providers_mu;
std::vector<std::function<void(std::string&)>>& providers() {
  static std::vector<std::function<void(std::string&)>> v;
  return v;
}
std::atomic<uint64_t> g_slow_us{0};
}  // namespace

void set_slow_request_threshold_us(uint64_t us) {
  g_slow_us.store(us, std::memory_order_relaxed);
}
uint64_t slow_request_threshold_us() { return g_slow_us.load(std::memory_order_relaxed); }

namespace {
// Microseconds as plain decimal seconds ("0.0001", "2.5", "5") — std::format's
// shortest-round-trip double would emit "1e-04"-style labels.
std::string seconds_text(uint64_t us) {
  std::string s = std::format("{}.{:06}", us / 1000000, us % 1000000);
  while (s.back() == '0') s.pop_back();
  if (s.back() == '.') s.pop_back();
  return s;
}
}  // namespace

void append_histogram(std::string& out, std::string_view name, std::string_view labels,
                      std::span<const uint64_t> bounds_us,
                      std::span<const uint64_t> buckets, uint64_t sum_us) {
  const char* sep = labels.empty() ? "" : ",";
  uint64_t cum = 0, count = 0;
  for (uint64_t b : buckets) count += b;
  for (size_t b = 0; b < bounds_us.size(); ++b) {
    cum += buckets[b];
    out += std::format("{}_bucket{{{}{}le=\"{}\"}} {}\n", name, labels, sep,
                       seconds_text(bounds_us[b]), cum);
  }
  out += std::format("{}_bucket{{{}{}le=\"+Inf\"}} {}\n", name, labels, sep, count);
  if (labels.empty()) {
    out += std::format("{}_sum {}\n{}_count {}\n", name, seconds_text(sum_us), name,
                       count);
  } else {
    out += std::format("{}_sum{{{}}} {}\n{}_count{{{}}} {}\n", name, labels,
                       seconds_text(sum_us), name, labels, count);
  }
}

void append_histogram(std::string& out, std::string_view name, std::string_view labels,
                      const LatencyHistogram::Snapshot& snap) {
  append_histogram(out, name, labels, LatencyHistogram::kBoundsUs, snap.buckets,
                   snap.sum_us);
}

void register_text_provider(std::function<void(std::string&)> provider) {
  std::lock_guard lock(g_providers_mu);
  providers().push_back(std::move(provider));
}

std::string prometheus_text() {
  auto& m = Metrics::instance();
  std::string out;
  out.reserve(16384);
  out += "# TYPE lightnfs_v3_calls_total counter\n";
  out += "# TYPE lightnfs_v3_errors_total counter\n";
  out += "# TYPE lightnfs_v3_duration_microseconds_total counter\n";
  out += "# TYPE lightnfs_v3_duration_seconds histogram\n";
  for (size_t i = 0; i < Metrics::kV3Procs; ++i) {
    uint64_t calls = m.v3_calls[i].load(std::memory_order_relaxed);
    if (calls == 0) continue;
    const char* name = v3_proc_name(static_cast<uint32_t>(i));
    out += std::format("lightnfs_v3_calls_total{{proc=\"{}\"}} {}\n", name, calls);
    out += std::format("lightnfs_v3_errors_total{{proc=\"{}\"}} {}\n", name,
                       m.v3_errors[i].load(std::memory_order_relaxed));
    out += std::format("lightnfs_v3_duration_microseconds_total{{proc=\"{}\"}} {}\n", name,
                       m.v3_duration_us[i].load(std::memory_order_relaxed));
    append_histogram(out, "lightnfs_v3_duration_seconds",
                     std::format("proc=\"{}\"", name), m.v3_duration[i].snapshot());
  }
  out += "# TYPE lightnfs_v4_op_calls_total counter\n";
  out += "# TYPE lightnfs_v4_op_errors_total counter\n";
  out += "# TYPE lightnfs_v4_op_duration_seconds histogram\n";
  for (size_t i = 0; i < Metrics::kV4Ops; ++i) {
    uint64_t calls = m.v4_op_calls[i].load(std::memory_order_relaxed);
    if (calls == 0) continue;
    const char* name = m.v4_op_names[i].load(std::memory_order_relaxed);
    std::string label = std::format("op=\"{}\"", name ? name : "?");
    out += std::format("lightnfs_v4_op_calls_total{{{}}} {}\n", label, calls);
    out += std::format("lightnfs_v4_op_errors_total{{{}}} {}\n", label,
                       m.v4_op_errors[i].load(std::memory_order_relaxed));
    append_histogram(out, "lightnfs_v4_op_duration_seconds", label,
                     m.v4_op_duration[i].snapshot());
  }
  out += "# TYPE lightnfs_v4_compound_duration_seconds histogram\n";
  append_histogram(out, "lightnfs_v4_compound_duration_seconds", "",
                   m.v4_compound_duration.snapshot());
  out += std::format("lightnfs_rpc_garbage_total {}\n",
                     m.rpc_garbage.load(std::memory_order_relaxed));
  out += std::format("lightnfs_mount_calls_total {}\n",
                     m.mount_calls.load(std::memory_order_relaxed));
  out += std::format("lightnfs_connections_accepted_total {}\n",
                     m.conns_accepted.load(std::memory_order_relaxed));
  out += std::format("lightnfs_connections_active {}\n",
                     m.conns_active.load(std::memory_order_relaxed));
  out += std::format("lightnfs_connections_rejected_total {}\n",
                     m.conns_rejected.load(std::memory_order_relaxed));
  out += std::format("lightnfs_backpressure_waits_total {}\n",
                     m.backpressure_waits.load(std::memory_order_relaxed));
  out += std::format("lightnfs_read_bytes_total {}\n",
                     m.read_bytes.load(std::memory_order_relaxed));
  out += std::format("lightnfs_write_bytes_total {}\n",
                     m.write_bytes.load(std::memory_order_relaxed));
  std::lock_guard lock(g_providers_mu);
  for (const auto& p : providers()) p(out);
  return out;
}

}  // namespace lnfs::obs
