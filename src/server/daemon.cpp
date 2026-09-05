#include "server/daemon.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "core/boot_epoch.hpp"
#include "core/config.hpp"
#include "core/file_handle.hpp"
#include "obs/errlog.hpp"
#include "obs/metrics.hpp"
#include "runtime/runtime.hpp"
#include "server/cluster_controller.hpp"
#include "server/cluster_store.hpp"
#include "server/data_plane.hpp"
#include "server/frontend.hpp"
#include "server/metrics_providers.hpp"
#include "server/protocol_stack.hpp"
#include "util/log.hpp"

namespace lnfs::server {
namespace {

// ---- phase 1: configuration ---------------------------------------------------------

// Parse + validate the TOML. nullopt after logging the reason.
std::optional<core::Config> load_validated_config(const std::string& path) {
  auto config = core::load_config(path);
  if (!config) {
    LNFS_ERROR("cannot load config {}: {}", path, errno_name(config.error()));
    return std::nullopt;
  }
  if (auto ok = core::validate_config(*config); !ok) {
    LNFS_ERROR("invalid config {}: {}", path, errno_name(ok.error()));
    return std::nullopt;
  }
  return std::move(*config);
}

void apply_log_level(const core::ServerConfig& cfg) {
  set_log_level(cfg.log_level == "debug"   ? LogLevel::kDebug
                : cfg.log_level == "warn"  ? LogLevel::kWarn
                : cfg.log_level == "error" ? LogLevel::kError
                                           : LogLevel::kInfo);
}

// Slow-request log threshold + error-sampling ring size (plan doc 10 §3.6/§3.7); both
// hot-reloadable, so this runs at startup and on every reload.
void apply_observability(const core::ServerConfig& cfg) {
  obs::set_slow_request_threshold_us(static_cast<uint64_t>(cfg.slow_request_ms) * 1000);
  obs::set_error_ring_capacity(cfg.error_ring);
}

// Per-client (v4 clientid) token buckets; hot-reloadable.
void apply_client_qos(ProtocolStack& stack, const core::ServerConfig& cfg) {
  if (stack.nfs4)
    stack.nfs4->configure_client_qos(cfg.client_read_bps, cfg.client_write_bps, cfg.client_iops);
}

// ---- phase 2: durable identity ------------------------------------------------------

// Where the handle key and the epoch come from (plan 10 A3): state_dir in
// single-gateway mode, the shared cluster store otherwise.  Both are resolved before
// the export table is built so a bad shared_dir fails fast, and both stay outside
// build_core_state so the caller decides when the epoch advances.
struct Identity {
  std::array<std::byte, 16> key{};
  uint64_t epoch = 0;
};

std::optional<Identity> local_identity(const std::string& state_dir) {
  std::error_code ec;
  std::filesystem::create_directories(state_dir, ec);
  if (ec) {
    LNFS_ERROR("cannot create state_dir {}: {}", state_dir, errno_name(errno_from(ec.value())));
    return std::nullopt;
  }
  auto key = core::load_or_create_hmac_key(state_dir + "/hmac.key");
  if (!key) {
    LNFS_ERROR("cannot load file-handle key: {}", errno_name(key.error()));
    return std::nullopt;
  }
  auto epoch = core::bump_boot_epoch(state_dir);
  if (!epoch) {
    LNFS_ERROR("cannot persist boot epoch: {}", errno_name(epoch.error()));
    return std::nullopt;
  }
  return Identity{*key, *epoch};
}

// Cluster mode (design 09 §9.3/§9.5): the key is shared, and the epoch is the global
// one — advanced by the ClusterController when this gateway takes over (plan 10 C2),
// never at process start.  The value read here only labels the standby; the stack is
// built with the epoch the takeover minted.
std::optional<Identity> cluster_identity(ClusterStore& store) {
  auto key = store.load_or_create_key();
  if (!key) {
    LNFS_ERROR("cannot load the cluster file-handle key: {}", errno_name(key.error()));
    return std::nullopt;
  }
  auto epoch = store.read_epoch();
  if (!epoch) {
    LNFS_ERROR("cannot read the cluster epoch: {}", errno_name(epoch.error()));
    return std::nullopt;
  }
  return Identity{*key, *epoch};
}

std::optional<CoreState> build_core_state(core::Config&& config, const Identity& identity,
                                          ClusterStore* cluster) {
  auto exports = core::ExportTable::build(std::move(config));
  if (!exports) {
    LNFS_ERROR("cannot initialize exports: {}", errno_name(exports.error()));
    return std::nullopt;
  }
  CoreState core{std::move(*exports), core::FileHandleCodec::from_key_only(identity.key),
                 identity.epoch, cluster};
  core.key.bind(*core.exports);
  return core;
}

// Multi-gateway failover prerequisites (design 09 §9.2, plan 10 A1): every export's
// backend must present cluster-stable handles and push byte-range locks into the
// storage, otherwise a second gateway cannot take the first one's clients over.  The
// capability bits only exist once the backends are constructed, so this runs after
// ExportTable::build rather than inside validate_config.  Returns false (reasons
// logged) unless the test-only escape hatch downgrades the failures to warnings.
bool check_cluster_backends(const core::ClusterConfig& cluster,
                            const core::ExportTable& exports) {
  if (!cluster.enabled) return true;
  bool ok = true;
  for (const auto& entry : exports.entries()) {
    auto caps = entry->backend->caps();
    std::string missing;
    if (!caps.has(backend::Cap::kStableHandles)) missing += " stable-handles";
    if (!caps.has(backend::Cap::kByteLocks)) missing += " byte-locks";
    if (!entry->backend->native_locks().has_value()) missing += " native_locks";
    if (missing.empty()) continue;
    if (cluster.unsafe_skip_backend_checks) {
      LNFS_WARN("export {}: cluster mode requires{} (skipped: unsafe_skip_backend_checks)",
                entry->path, missing);
    } else {
      LNFS_ERROR("export {}: cluster mode requires{}", entry->path, missing);
      ok = false;
    }
  }
  return ok;
}

// Export-table consistency across the cluster (design 09 §9.3, plan 10 B4): refuse to
// join if any other node's digest differs — the same fsid over a different tree would
// hand clients handles that resolve to different files after a takeover — and publish
// this node's digest only once it agrees, so a misconfigured node never leaves a
// record that would keep the healthy ones from restarting.  A removed node's stale
// exports.<node> record must be deleted by the operator (no automatic GC).
bool check_exports_consistency(ClusterStore& store, const std::string& node,
                               const std::string& digest) {
  auto listed = store.list_exports_digests();
  if (!listed) {
    LNFS_ERROR("cannot read the cluster export digests: {}", errno_name(listed.error()));
    return false;
  }
  bool ok = true;
  for (const auto& [peer, theirs] : *listed) {
    if (peer == node || theirs == digest) continue;
    LNFS_ERROR("export table differs from cluster node {}: ours {} theirs {} "
               "(fix the config, or delete shared_dir/exports.{} if that node is gone)",
               peer, digest, theirs, peer);
    ok = false;
  }
  if (!ok) return false;
  if (auto put = store.put_exports_digest(node, digest); !put) {
    LNFS_ERROR("cannot publish the export digest to the cluster store: {}",
               errno_name(put.error()));
    return false;
  }
  return true;
}

// `--check-config` must not write into shared_dir (another gateway may be active);
// it only reports whether this gateway could.
void warn_shared_dir_access(const core::ClusterConfig& cluster) {
  if (!cluster.enabled) return;
  if (::access(cluster.shared_dir.c_str(), W_OK | X_OK) < 0)
    LNFS_WARN("cluster shared_dir {} is not writable from this host: {}",
              cluster.shared_dir, errno_name(errno_from(errno)));
}

// ---- phase 3: runtime + backends ----------------------------------------------------

rt::Runtime::Config runtime_config(const core::ServerConfig& cfg) {
  return {.reactors = cfg.reactors,
          .offload_threads = cfg.offload_threads,
          .offload_heavy_threads = cfg.offload_heavy_threads,
          .offload_queue_cap = cfg.offload_queue_cap,
          .ring = cfg.ring,
          .ring_sqpoll = cfg.ring_sqpoll};
}

// Runs a backend lifecycle coroutine on `reactor` and blocks the calling (main) thread
// until it completes — start()/stop() are the only backend calls made off-reactor.
Result<void> run_on_reactor(rt::Reactor& reactor, rt::Task<Result<void>> task) {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  Result<void> result;
  rt::spawn(
      [](rt::Task<Result<void>> work, std::mutex* mu, std::condition_variable* cv,
         bool* done, Result<void>* result) -> rt::Task<void> {
        *result = co_await std::move(work);
        {
          std::lock_guard lock(*mu);
          *done = true;
          cv->notify_one();  // under the lock: the waiter cannot destroy cv first
        }
      }(std::move(task), &mu, &cv, &done, &result),
      reactor);
  std::unique_lock lock(mu);
  cv.wait(lock, [&] { return done; });
  return result;
}

// One line per export for the v4.2 probe and the capability bits the engines consume
// (plan doc 10 §5.3): native change counter, storage-side access, native locks, jukebox.
void log_backend_traits(const core::ExportEntry& entry) {
  auto caps = entry.backend->caps();
  LNFS_INFO("export {} v4.2 capabilities: seek/allocate={} copy={} clone={}", entry.path,
            caps.has(backend::Cap::kSparseOps), caps.has(backend::Cap::kCopyRange),
            caps.has(backend::Cap::kCloneRange));
  LNFS_INFO("export {} backend traits: stable-handles={} native-change={} "
            "native-access={} native-locks={} jukebox={}",
            entry.path, caps.has(backend::Cap::kStableHandles),
            caps.has(backend::Cap::kNativeChange), caps.has(backend::Cap::kNativeAccess),
            entry.backend->native_locks().has_value(), caps.has(backend::Cap::kJukebox));
}

// Start every backend on reactor 0 (cluster backends connect here).
bool start_backends(rt::Runtime& runtime, core::ExportTable& exports) {
  for (const auto& entry : exports.entries()) {
    auto started = run_on_reactor(runtime.reactor(0), entry->backend->start());
    if (!started) {
      LNFS_ERROR("backend {} failed to start: {}", entry->path, errno_name(started.error()));
      return false;
    }
    log_backend_traits(*entry);
  }
  return true;
}

void stop_backends(rt::Runtime& runtime, core::ExportTable& exports) {
  for (const auto& entry : exports.entries())
    (void)run_on_reactor(runtime.reactor(0), entry->backend->stop());
}

// ---- hot reload (plan doc 10 §4.1, step 1) -----------------------------------------

// Topology/runtime keys stay fixed until restart; name what a reload ignored.
std::string restart_required_report(const core::ServerConfig& fresh,
                                    const core::ServerConfig& running) {
  std::string report;
  if (fresh.port != running.port || fresh.mount_port != running.mount_port ||
      fresh.bind != running.bind)
    report += "listen address/ports changed: restart required\n";
  if (fresh.reactors != running.reactors || fresh.offload_threads != running.offload_threads)
    report += "thread topology changed: restart required\n";
  if (fresh.state_dir != running.state_dir || fresh.enable_v4 != running.enable_v4 ||
      fresh.lease_seconds != running.lease_seconds ||
      fresh.state_shards != running.state_shards)
    report += "state_dir/v4/lease/shards changed: restart required\n";
  if (fresh.metrics_port != running.metrics_port || fresh.metrics_bind != running.metrics_bind)
    report += "metrics endpoint changed: restart required\n";
  return report;
}

// The [cluster] section fixes the gateway's identity, epoch source and fence: none of it
// can change under a live role (plan 10 A1).
std::string cluster_restart_required_report(const core::ClusterConfig& fresh,
                                            const core::ClusterConfig& running) {
  return fresh == running ? "" : "cluster settings changed: restart required\n";
}

// Re-parses the config file and applies the non-topology subset — log level,
// slow-request threshold, error ring, per-export client allowlists + QoS, per-client
// QoS.  Serialized by construction: SIGHUP applies on the main wait loop, `ctl reload`
// on the ctl reactor; both funnel through this one function and concurrent invocations
// are harmless (last writer wins on independent knobs).  Returns the ctl report text.
// `stack` may be null while no protocol stack exists (the management plane is up
// before the engines, plan 10 A4): the per-client QoS knobs are then skipped.
std::string reload_config(const std::string& config_path, const core::ServerConfig& running,
                          const core::ClusterConfig& running_cluster, CoreState& core,
                          ProtocolStack* stack) {
  auto fresh = load_validated_config(config_path);
  if (!fresh) return "reload failed: config invalid (details in the log)\n";
  std::string report;
  const auto& sc = fresh->server;
  if (sc.log_level != running.log_level) {
    apply_log_level(sc);
    report += std::format("log_level -> {}\n", sc.log_level);
  }
  apply_observability(sc);
  if (stack) apply_client_qos(*stack, sc);
  report += core.exports->reload_dynamic(*fresh);
  report += restart_required_report(sc, running);
  report += cluster_restart_required_report(fresh->cluster, running_cluster);
  LNFS_INFO("configuration reloaded from {}", config_path);
  return report.empty() ? "nothing to apply\n" : report;
}

// How long a stopping gateway lets established connections finish before closing them.
constexpr std::chrono::milliseconds kShutdownDrainGrace{2000};

// ---- signals -----------------------------------------------------------------------

volatile std::sig_atomic_t g_stopping = 0;
volatile std::sig_atomic_t g_reload_requested = 0;
void on_stop_signal(int) { g_stopping = 1; }
void on_sighup(int) { g_reload_requested = 1; }

// The main thread's event loop (plan 10 C2): runs until SIGINT/SIGTERM, applying
// SIGHUP reloads and whatever other threads post — the cluster controller's activate /
// deactivate work runs here because the data plane (Frontend::start, backend
// lifecycle calls) belongs to the main thread.
class MainLoop {
 public:
  void post(std::function<void()> fn) {
    {
      std::lock_guard lock(mu_);
      queue_.push_back(std::move(fn));
    }
    cv_.notify_one();
  }
  void run(const std::function<void()>& on_reload) {
    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);
    std::signal(SIGHUP, on_sighup);
    while (!g_stopping) {
      if (g_reload_requested) {
        g_reload_requested = 0;
        on_reload();
      }
      std::function<void()> work;
      {
        std::unique_lock lock(mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return !queue_.empty(); });
        if (!queue_.empty()) {
          work = std::move(queue_.front());
          queue_.pop_front();
        }
      }
      if (work) work();
    }
  }
  // Runs what is still queued (a takeover posted just before the stop signal).
  void drain() {
    for (;;) {
      std::function<void()> work;
      {
        std::lock_guard lock(mu_);
        if (queue_.empty()) return;
        work = std::move(queue_.front());
        queue_.pop_front();
      }
      work();
    }
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
};

// One log line for a SIGHUP reload: the multi-line ctl report folded onto one line.
void log_reload_report(std::string report) {
  while (!report.empty() && report.back() == '\n') report.pop_back();
  std::replace(report.begin(), report.end(), '\n', ';');
  LNFS_INFO("reload (SIGHUP): {}", report);
}

}  // namespace

int check_config(const std::string& config_path) {
  auto config = load_validated_config(config_path);
  if (!config) return 1;
  // Also constructs the backends so per-backend keys ([export.local] identity, ...)
  // are validated exactly as a real startup would.
  const core::ClusterConfig cluster_cfg = config->cluster;
  auto exports = core::ExportTable::build(std::move(*config));
  if (!exports) {
    LNFS_ERROR("invalid config {}: {}", config_path, errno_name(exports.error()));
    return 1;
  }
  if (!check_cluster_backends(cluster_cfg, **exports)) {
    LNFS_ERROR("invalid config {}: backends do not meet the cluster requirements",
               config_path);
    return 1;
  }
  warn_shared_dir_access(cluster_cfg);
  std::printf("configuration is valid\n");
  return 0;
}

int run_server(const std::string& config_path) {
  // 1. configuration
  auto config = load_validated_config(config_path);
  if (!config) return 1;
  const core::ServerConfig server_cfg = config->server;
  const core::ClusterConfig cluster_cfg = config->cluster;
  const std::string exports_digest = core::canonical_exports_digest(*config);
  apply_log_level(server_cfg);

  // 2. durable identity: handle HMAC key + epoch (state_dir, or the shared cluster
  //    store), then the export table
  std::unique_ptr<ClusterStore> cluster_store;
  if (cluster_cfg.enabled)
    cluster_store = make_posix_cluster_store(
        cluster_cfg.shared_dir, std::chrono::milliseconds(2 * cluster_cfg.fence_lease_ms));
  auto identity = cluster_store ? cluster_identity(*cluster_store)
                                : local_identity(server_cfg.state_dir);
  if (!identity) return 1;
  auto core = build_core_state(std::move(*config), *identity, cluster_store.get());
  if (!core) return 1;
  if (!check_cluster_backends(cluster_cfg, *core->exports)) return 1;
  if (cluster_store) {
    const std::string node = core::cluster_node_name(cluster_cfg);
    if (!check_exports_consistency(*cluster_store, node, exports_digest)) return 1;
    LNFS_INFO("cluster mode: id={} node={} shared_dir={} epoch={} exports={}", cluster_cfg.id,
              node, cluster_cfg.shared_dir, core->epoch, exports_digest);
  }
  init_async_logging({.file = server_cfg.log_file,
                      .rotate_size = server_cfg.log_rotate_size,
                      .rotate_keep = server_cfg.log_rotate_keep});

  // 3. runtime + backends
  rt::Runtime runtime(runtime_config(server_cfg));
  runtime.start();
  if (!start_backends(runtime, *core->exports)) {
    runtime.stop_and_join();
    return 1;
  }

  // 3b. management plane (ctl socket + metrics endpoint): up before the engines and
  //     down after them, so it answers while no data plane exists (plan 10 A4).
  std::atomic<ProtocolStack*> active_stack{nullptr};
  auto do_reload = [config_path, server_cfg, cluster_cfg, &core, &active_stack]() -> std::string {
    return reload_config(config_path, server_cfg, cluster_cfg, *core,
                         active_stack.load(std::memory_order_acquire));
  };
  MainLoop loop;
  std::optional<DataPlaneInstance> plane;
  std::unique_ptr<ClusterController> controller;
  Management mgmt = Management::start(server_cfg, runtime, do_reload, [&]() -> std::string {
    return controller ? role_name(controller->role()) : (plane ? "active" : "standby");
  });
  apply_observability(server_cfg);
  // The data-plane hooks the single gateway and the controller share (main thread).
  auto bring_up = [&](uint64_t epoch) -> Result<void> {
    core->epoch = epoch;
    plane = activate(server_cfg, cluster_cfg, *core, runtime, mgmt);
    if (!plane) return Err(errno_from(EIO));
    active_stack.store(plane->stack.get(), std::memory_order_release);
    LNFS_INFO("lightnfs {} ready: nfs_port={} mount_port={} exports={} epoch={}",
              LIGHTNFS_VERSION, plane->frontend->nfs->port(), plane->frontend->mount->port(),
              core->exports->entries().size(), epoch);
    return {};
  };
  auto take_down = [&](std::chrono::milliseconds grace) {
    if (!plane) return;
    active_stack.store(nullptr);
    (void)deactivate(*plane, server_cfg, mgmt, grace);
    plane.reset();
  };

  if (!cluster_store) {
    // 4+5. single gateway (plan 10 C1): the data plane once, for the whole process.
    if (!bring_up(core->epoch)) {
      mgmt.stop();
      runtime.stop_and_join();
      return 1;
    }
  } else {
    // 4+5. cluster (plan 10 C2): standby until the controller takes the fence; the
    //      data plane is built with the epoch the takeover mints and torn down again
    //      when the fence is lost or the operator asks.
    const std::chrono::milliseconds drain_grace(2 * cluster_cfg.fence_lease_ms);
    ClusterController::Hooks hooks;
    hooks.post = [&loop](std::function<void()> fn) { loop.post(std::move(fn)); };
    hooks.activate = bring_up;
    hooks.deactivate = [&, drain_grace] { take_down(drain_grace); };
    hooks.backend_reset = [&] {
      stop_backends(runtime, *core->exports);
      if (!start_backends(runtime, *core->exports))
        LNFS_ERROR("cluster: backends failed to restart after draining; the next takeover "
                   "will not serve");
    };
    controller = std::make_unique<ClusterController>(cluster_cfg, *cluster_store,
                                                     std::move(hooks));
    controller->start();
    LNFS_INFO("lightnfs {} standby: node={} role={} takeover={} fence_lease={}ms",
              LIGHTNFS_VERSION, core::cluster_node_name(cluster_cfg), cluster_cfg.role,
              cluster_cfg.takeover, cluster_cfg.fence_lease_ms);
  }

  loop.run([&] { log_reload_report(do_reload()); });

  // mirror-image shutdown: controller timer → pending posted work → data plane
  // (detach from ctl → stop accepting → connections → lease scanner → stack) → fence
  // → backends → management → runtime → logging
  if (controller) controller->stop();
  loop.drain();
  take_down(kShutdownDrainGrace);
  if (controller && controller->role() != Role::kStandby) {
    (void)cluster_store->release_fence(core::cluster_node_name(cluster_cfg));
    LNFS_INFO("cluster: fence released on exit");
  }
  stop_backends(runtime, *core->exports);
  mgmt.stop();
  runtime.stop_and_join();
  LNFS_INFO("lightnfs stopped");
  shutdown_async_logging();
  return 0;
}

}  // namespace lnfs::server
