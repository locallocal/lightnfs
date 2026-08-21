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
}  // namespace

void register_text_provider(std::function<void(std::string&)> provider) {
  std::lock_guard lock(g_providers_mu);
  providers().push_back(std::move(provider));
}

std::string prometheus_text() {
  auto& m = Metrics::instance();
  std::string out;
  out.reserve(4096);
  out += "# TYPE lightnfs_v3_calls_total counter\n";
  out += "# TYPE lightnfs_v3_errors_total counter\n";
  out += "# TYPE lightnfs_v3_duration_microseconds_total counter\n";
  for (size_t i = 0; i < Metrics::kV3Procs; ++i) {
    uint64_t calls = m.v3_calls[i].load(std::memory_order_relaxed);
    if (calls == 0) continue;
    const char* name = v3_proc_name(static_cast<uint32_t>(i));
    out += std::format("lightnfs_v3_calls_total{{proc=\"{}\"}} {}\n", name, calls);
    out += std::format("lightnfs_v3_errors_total{{proc=\"{}\"}} {}\n", name,
                       m.v3_errors[i].load(std::memory_order_relaxed));
    out += std::format("lightnfs_v3_duration_microseconds_total{{proc=\"{}\"}} {}\n", name,
                       m.v3_duration_us[i].load(std::memory_order_relaxed));
  }
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
