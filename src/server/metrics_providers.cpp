#include "server/metrics_providers.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <string>

#include "backend/cephfs/cephfs.hpp"
#include "backend/gluster/gluster.hpp"
#include "backend/local/local.hpp"
#include "backend/lustre/lustre.hpp"
#include "core/config.hpp"
#include "obs/metrics.hpp"
#include "rpc/drc.hpp"
#include "runtime/runtime.hpp"
#include "state/state_mgr.hpp"

namespace lnfs::server {
namespace {

void append_drc(std::string& out, rpc::Drc& drc) {
  auto s = drc.stats();
  out += std::format(
      "lightnfs_drc_inserts_total {}\nlightnfs_drc_replays_total {}\n"
      "lightnfs_drc_waits_total {}\nlightnfs_drc_evictions_total {}\n"
      "lightnfs_drc_entries {}\nlightnfs_drc_bytes {}\n",
      s.inserts, s.replays, s.waits, s.evictions, s.entries, s.bytes);
}

void append_v4_state(std::string& out, state::StateMgr& state) {
  auto s = state.stats();
  out += std::format(
      "lightnfs_v4_clients {}\nlightnfs_v4_sessions {}\nlightnfs_v4_opens {}\n"
      "lightnfs_v4_seq_new_total {}\nlightnfs_v4_seq_replay_total {}\n"
      "lightnfs_v4_seq_misordered_total {}\nlightnfs_v4_seq_waits_total {}\n"
      "lightnfs_v4_in_grace {}\nlightnfs_v4_grace_remaining_seconds {}\n"
      "lightnfs_v4_files_with_state {}\nlightnfs_v4_courtesy_clients {}\n"
      "lightnfs_v4_lease_expirations_total {}\n"
      "lightnfs_v4_reclaims_total{{reason=\"conflict\"}} {}\n"
      "lightnfs_v4_reclaims_total{{reason=\"timeout\"}} {}\n"
      "lightnfs_v4_reclaims_total{{reason=\"forced\"}} {}\n"
      "lightnfs_v4_share_denied_total {}\nlightnfs_v4_open_merges_total {}\n"
      // Lock-state gauges promised by deployment.md (plan doc 10 §3.4).
      "lightnfs_v4_lock_states {}\nlightnfs_v4_lock_segments {}\n"
      "lightnfs_v4_lock_owners {}\nlightnfs_v4_lock_denied_total {}\n"
      // Delegations + backchannel (plan doc 10 §5.2).
      "lightnfs_v4_delegations {}\nlightnfs_v4_deleg_grants_total {}\n"
      "lightnfs_v4_deleg_recalls_total {}\nlightnfs_v4_deleg_returns_total {}\n"
      "lightnfs_v4_deleg_revokes_total {}\nlightnfs_v4_cb_lock_notifies_total {}\n"
      // Native lock push (plan doc 10 §5.3).
      "lightnfs_v4_native_lock_denied_total {}\nlightnfs_v4_native_lock_errors_total {}\n",
      s.clients, s.sessions, s.opens, s.seq_new, s.seq_replay, s.seq_misordered,
      s.seq_waits, s.grace ? 1 : 0, s.grace_remaining, s.files, s.courtesy,
      s.lease_expirations, s.reclaim_conflict, s.reclaim_timeout, s.reclaim_forced,
      s.share_denied, s.open_merges, s.lock_states, s.lock_segments, s.lock_owners,
      s.lock_denied, s.delegs, s.deleg_grants, s.deleg_recalls, s.deleg_returns,
      s.deleg_revokes, s.cb_lock_notifies, s.native_lock_denied, s.native_lock_errors);
}

// ---- per-backend cache / jukebox / lock-handle counters --------------------------

// Gluster backend caches + jukebox/lock-descriptor counters (design 06 §6.6).
void append_gluster(std::string& out, const std::string& labels,
                    const backend::GlusterBackend& g) {
  auto s = g.stats();
  out += std::format(
      "lightnfs_gluster_fdcache_hits_total{{{0}}} {1}\n"
      "lightnfs_gluster_fdcache_misses_total{{{0}}} {2}\n"
      "lightnfs_gluster_fdcache_upgrades_total{{{0}}} {3}\n"
      "lightnfs_gluster_fdcache_evictions_total{{{0}}} {4}\n"
      "lightnfs_gluster_fdcache_entries{{{0}}} {5}\n"
      "lightnfs_gluster_objcache_hits_total{{{0}}} {6}\n"
      "lightnfs_gluster_objcache_misses_total{{{0}}} {7}\n"
      "lightnfs_gluster_objcache_entries{{{0}}} {8}\n"
      "lightnfs_gluster_jukebox_total{{{0}}} {9}\n"
      "lightnfs_gluster_lock_fds{{{0}}} {10}\n",
      labels, s.fd_hits, s.fd_misses, s.fd_upgrades, s.fd_evictions, s.fd_entries,
      s.obj_hits, s.obj_misses, s.obj_entries, s.jukebox, s.lock_fds);
}

// CephFS backend caches + jukebox/blocklist/lock-handle counters (design 06 §6.8).
void append_cephfs(std::string& out, const std::string& labels,
                   const backend::CephBackend& c) {
  auto s = c.stats();
  out += std::format(
      "lightnfs_cephfs_fdcache_hits_total{{{0}}} {1}\n"
      "lightnfs_cephfs_fdcache_misses_total{{{0}}} {2}\n"
      "lightnfs_cephfs_fdcache_upgrades_total{{{0}}} {3}\n"
      "lightnfs_cephfs_fdcache_evictions_total{{{0}}} {4}\n"
      "lightnfs_cephfs_fdcache_entries{{{0}}} {5}\n"
      "lightnfs_cephfs_objcache_hits_total{{{0}}} {6}\n"
      "lightnfs_cephfs_objcache_misses_total{{{0}}} {7}\n"
      "lightnfs_cephfs_objcache_entries{{{0}}} {8}\n"
      "lightnfs_cephfs_jukebox_total{{{0}}} {9}\n"
      "lightnfs_cephfs_blocklisted_total{{{0}}} {10}\n"
      "lightnfs_cephfs_lock_fds{{{0}}} {11}\n",
      labels, s.fd_hits, s.fd_misses, s.fd_upgrades, s.fd_evictions, s.fd_entries,
      s.obj_hits, s.obj_misses, s.obj_entries, s.jukebox, s.blocklisted, s.lock_fds);
}

// Lustre extras (design 06 §6.5): HSM gate + native lock descriptors; the fd /
// resolve cache counters come from the local-backend rows it inherits.
void append_lustre(std::string& out, const std::string& labels,
                   const backend::LustreBackend& l) {
  auto s = l.stats();
  out += std::format(
      "lightnfs_lustre_jukebox_total{{{0}}} {1}\n"
      "lightnfs_lustre_hsm_checks_total{{{0}}} {2}\n"
      "lightnfs_lustre_hsm_restores_total{{{0}}} {3}\n"
      "lightnfs_lustre_lock_fds{{{0}}} {4}\n",
      labels, s.jukebox, s.hsm_checks, s.hsm_restores, s.lock_fds);
}

// Local-backend fd / O_PATH resolve cache counters (plan doc 10 §3.5).
void append_local(std::string& out, const std::string& labels,
                  const backend::LocalBackend& local) {
  auto s = local.fd_cache_stats();
  out += std::format(
      "lightnfs_fdcache_hits_total{{{0}}} {1}\n"
      "lightnfs_fdcache_misses_total{{{0}}} {2}\n"
      "lightnfs_fdcache_upgrades_total{{{0}}} {3}\n"
      "lightnfs_fdcache_evictions_total{{{0}}} {4}\n"
      "lightnfs_fdcache_overflows_total{{{0}}} {5}\n"
      "lightnfs_fdcache_entries{{{0}}} {6}\n"
      "lightnfs_fdcache_path_hits_total{{{0}}} {7}\n"
      "lightnfs_fdcache_path_misses_total{{{0}}} {8}\n"
      "lightnfs_fdcache_path_entries{{{0}}} {9}\n",
      labels, s.hits, s.misses, s.upgrades, s.evictions, s.overflows, s.entries,
      s.path_hits, s.path_misses, s.path_entries);
}

// Per-export data-path counters with {export,fsid} labels, followed by whatever the
// export's backend exposes. Lustre inherits the local cache rows, so it gets both.
void append_exports(std::string& out, core::ExportTable& exports) {
  for (const auto& entry : exports.entries()) {
    std::string labels = std::format("export=\"{}\",fsid=\"{}\"", entry->path, entry->fsid);
    const auto& em = entry->metrics;
    out += std::format(
        "lightnfs_export_read_bytes_total{{{0}}} {1}\n"
        "lightnfs_export_write_bytes_total{{{0}}} {2}\n"
        "lightnfs_export_read_ops_total{{{0}}} {3}\n"
        "lightnfs_export_write_ops_total{{{0}}} {4}\n",
        labels, em.read_bytes.load(), em.write_bytes.load(), em.read_ops.load(),
        em.write_ops.load());
    backend::Backend* be = entry->backend.get();
    if (auto* g = dynamic_cast<backend::GlusterBackend*>(be)) {
      append_gluster(out, labels, *g);
    } else if (auto* c = dynamic_cast<backend::CephBackend*>(be)) {
      append_cephfs(out, labels, *c);
    } else if (auto* local = dynamic_cast<backend::LocalBackend*>(be)) {
      if (auto* l = dynamic_cast<backend::LustreBackend*>(local)) append_lustre(out, labels, *l);
      append_local(out, labels, *local);
    }
  }
}

// Runtime layer (plan doc 10 §2.5/§3.5): offload queue depth and per-class throughput,
// plus the reactor loop busy-period histogram aggregated over all reactors.
void append_runtime(std::string& out, rt::Runtime& runtime) {
  auto s = runtime.offload().stats();
  static constexpr const char* kCls[] = {"light", "heavy"};
  for (int c = 0; c < rt::kOffloadClasses; ++c) {
    out += std::format(
        "lightnfs_offload_queue_depth{{class=\"{}\"}} {}\n"
        "lightnfs_offload_submitted_total{{class=\"{}\"}} {}\n"
        "lightnfs_offload_completed_total{{class=\"{}\"}} {}\n"
        "lightnfs_offload_admission_deferred_total{{class=\"{}\"}} {}\n",
        kCls[c], s.depth[c], kCls[c], s.submitted[c], kCls[c], s.completed[c], kCls[c],
        s.deferred[c]);
  }
  std::array<uint64_t, rt::Reactor::kLoopBuckets> agg{};
  uint64_t sum_us = 0;
  for (size_t i = 0; i < runtime.reactor_count(); ++i) {
    auto ls = runtime.reactor(i).loop_stats();
    for (size_t b = 0; b < agg.size(); ++b) agg[b] += ls.buckets[b];
    sum_us += ls.sum_us;
  }
  out += "# TYPE lightnfs_reactor_loop_duration_seconds histogram\n";
  obs::append_histogram(out, "lightnfs_reactor_loop_duration_seconds", "",
                        rt::Reactor::kLoopBoundsUs, agg, sum_us);
}

}  // namespace

void register_metrics_providers(const MetricsSources& src) {
  obs::register_text_provider([&drc = src.drc](std::string& out) { append_drc(out, drc); });
  obs::register_text_provider(
      [&state = src.state](std::string& out) { append_v4_state(out, state); });
  obs::register_text_provider(
      [&exports = src.exports](std::string& out) { append_exports(out, exports); });
  obs::register_text_provider(
      [&runtime = src.runtime](std::string& out) { append_runtime(out, runtime); });
}

}  // namespace lnfs::server
