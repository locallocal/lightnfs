#include "server/daemon.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
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
#include "server/cluster_store.hpp"
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
// one.  Until the ClusterController (plan 10 C2) moves the bump into the takeover
// step, every process start still advances it here so stateids from a previous
// incarnation are never mistaken for live ones.
std::optional<Identity> cluster_identity(ClusterStore& store) {
  auto key = store.load_or_create_key();
  if (!key) {
    LNFS_ERROR("cannot load the cluster file-handle key: {}", errno_name(key.error()));
    return std::nullopt;
  }
  auto epoch = store.bump_epoch();
  if (!epoch) {
    LNFS_ERROR("cannot advance the cluster epoch: {}", errno_name(epoch.error()));
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

// ---- signals -----------------------------------------------------------------------

volatile std::sig_atomic_t g_stopping = 0;
volatile std::sig_atomic_t g_reload_requested = 0;
void on_stop_signal(int) { g_stopping = 1; }
void on_sighup(int) { g_reload_requested = 1; }

// Blocks the main thread until SIGINT/SIGTERM; SIGHUP runs `on_reload` here.
void wait_for_shutdown_signal(const std::function<void()>& on_reload) {
  std::signal(SIGINT, on_stop_signal);
  std::signal(SIGTERM, on_stop_signal);
  std::signal(SIGHUP, on_sighup);
  while (!g_stopping) {
    if (g_reload_requested) {
      g_reload_requested = 0;
      on_reload();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

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
  if (cluster_store)
    LNFS_INFO("cluster mode: id={} node={} shared_dir={} epoch={}", cluster_cfg.id,
              core::cluster_node_name(cluster_cfg), cluster_cfg.shared_dir, core->epoch);
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
  Management mgmt = Management::start(server_cfg, runtime, do_reload);

  // 4. protocol engines, metrics providers, observability + QoS knobs
  ProtocolStack stack(server_cfg, *core);
  if (server_cfg.enable_v4) stack.enable_v4(server_cfg, cluster_cfg, *core, runtime);
  register_metrics_providers(
      {.drc = stack.drc, .state = stack.state, .exports = *core->exports, .runtime = runtime});
  apply_observability(server_cfg);
  apply_client_qos(stack, server_cfg);
  active_stack.store(&stack, std::memory_order_release);

  // 5. frontend (listeners, data plane attached to ctl, rpcbind)
  auto frontend = Frontend::start(server_cfg, runtime, stack, *core, mgmt);
  if (!frontend) {
    active_stack.store(nullptr);
    mgmt.stop();
    runtime.stop_and_join();
    return 1;
  }
  LNFS_INFO("lightnfs {} ready: nfs_port={} mount_port={} exports={}", LIGHTNFS_VERSION,
            frontend->nfs->port(), frontend->mount->port(), core->exports->entries().size());

  wait_for_shutdown_signal([&] { log_reload_report(do_reload()); });

  // mirror-image shutdown: lease scanner → frontend (detaches the data plane) →
  // backends → management → runtime → logging
  stack.lease_stop.store(true);
  frontend->stop(server_cfg, mgmt);
  active_stack.store(nullptr);
  stop_backends(runtime, *core->exports);
  mgmt.stop();
  runtime.stop_and_join();
  LNFS_INFO("lightnfs stopped");
  shutdown_async_logging();
  return 0;
}

}  // namespace lnfs::server
