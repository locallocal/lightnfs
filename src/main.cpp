#include <sys/stat.h>
#include <unistd.h>

#include <ccmd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <condition_variable>
#include <cstdio>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/boot_epoch.hpp"
#include "core/config.hpp"
#include "obs/errlog.hpp"
#include "obs/metrics.hpp"
#include "runtime/runtime.hpp"
#include "server/frontend.hpp"
#include "server/metrics_providers.hpp"
#include "server/protocol_stack.hpp"
#include "util/log.hpp"

namespace {

volatile std::sig_atomic_t stopping = 0;
void on_signal(int) { stopping = 1; }
// SIGHUP = hot reload (plan doc 10 §4.1); applied on the main thread's wait loop.
volatile std::sig_atomic_t reload_requested = 0;
void on_sighup(int) { reload_requested = 1; }

lnfs::Result<void> run_backend_hook(lnfs::rt::Reactor& reactor,
                                    lnfs::rt::Task<lnfs::Result<void>> task) {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  lnfs::Result<void> result;
  lnfs::rt::spawn(
      [](lnfs::rt::Task<lnfs::Result<void>> work, std::mutex* mu,
         std::condition_variable* cv, bool* done,
         lnfs::Result<void>* result) -> lnfs::rt::Task<void> {
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

// Parse + validate the TOML. nullopt after logging the reason.
std::optional<lnfs::core::Config> load_validated_config(const std::string& path) {
  auto config = lnfs::core::load_config(path);
  if (!config) {
    LNFS_ERROR("cannot load config {}: {}", path, lnfs::errno_name(config.error()));
    return std::nullopt;
  }
  if (auto ok = lnfs::core::validate_config(*config); !ok) {
    LNFS_ERROR("invalid config {}: {}", path, lnfs::errno_name(ok.error()));
    return std::nullopt;
  }
  return std::move(*config);
}

void apply_log_level(const lnfs::core::ServerConfig& cfg) {
  lnfs::set_log_level(cfg.log_level == "debug"  ? lnfs::LogLevel::kDebug
                      : cfg.log_level == "warn" ? lnfs::LogLevel::kWarn
                      : cfg.log_level == "error"
                          ? lnfs::LogLevel::kError
                          : lnfs::LogLevel::kInfo);
}

using lnfs::server::CoreState;
using lnfs::server::ProtocolStack;

std::optional<CoreState> build_core_state(lnfs::core::Config&& config) {
  const std::string state_dir = config.server.state_dir;
  auto exports = lnfs::core::ExportTable::build(std::move(config));
  if (!exports) {
    LNFS_ERROR("cannot initialize exports: {}", lnfs::errno_name(exports.error()));
    return std::nullopt;
  }
  auto key = lnfs::core::FileHandleCodec::load_or_create(state_dir);
  if (!key) {
    LNFS_ERROR("cannot load file-handle key: {}", lnfs::errno_name(key.error()));
    return std::nullopt;
  }
  auto epoch = lnfs::core::bump_boot_epoch(state_dir);
  if (!epoch) {
    LNFS_ERROR("cannot persist boot epoch: {}", lnfs::errno_name(epoch.error()));
    return std::nullopt;
  }
  CoreState core{std::move(*exports), std::move(*key), *epoch};
  core.key.bind(*core.exports);
  return core;
}

// Start every backend on reactor 0 and log its v4.2 capability probe.
bool start_backends(lnfs::rt::Runtime& runtime, lnfs::core::ExportTable& exports) {
  for (const auto& entry : exports.entries()) {
    auto started = run_backend_hook(runtime.reactor(0), entry->backend->start());
    if (!started) {
      LNFS_ERROR("backend {} failed to start: {}", entry->path,
                 lnfs::errno_name(started.error()));
      return false;
    }
    auto caps = entry->backend->caps();
    LNFS_INFO("export {} v4.2 capabilities: seek/allocate={} copy={} clone={}", entry->path,
              caps.has(lnfs::backend::Cap::kSparseOps), caps.has(lnfs::backend::Cap::kCopyRange),
              caps.has(lnfs::backend::Cap::kCloneRange));
    // The half-wired bits of plan doc 10 §5.3, now consumed: native change counter
    // (change_attr_type), storage-side access, native byte locks, jukebox.
    LNFS_INFO("export {} backend traits: stable-handles={} native-change={} "
              "native-access={} native-locks={} jukebox={}",
              entry->path, caps.has(lnfs::backend::Cap::kStableHandles),
              caps.has(lnfs::backend::Cap::kNativeChange),
              caps.has(lnfs::backend::Cap::kNativeAccess),
              entry->backend->native_locks().has_value(),
              caps.has(lnfs::backend::Cap::kJukebox));
  }
  return true;
}

void stop_backends(lnfs::rt::Runtime& runtime, lnfs::core::ExportTable& exports) {
  for (const auto& entry : exports.entries())
    (void)run_backend_hook(runtime.reactor(0), entry->backend->stop());
}

void wait_for_shutdown_signal(const std::function<void()>& on_reload) {
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGHUP, on_sighup);  // hot reload (plan doc 10 §4.1)
  while (!stopping) {
    if (reload_requested) {
      reload_requested = 0;
      on_reload();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

// ---- run_server ----------------------------------------------------------------
// Startup order (config → identity → backends → engines → frontend) and the
// mirror-image shutdown; each phase is one of the self-contained helpers above
// (load_validated_config / build_core_state / start_backends, ProtocolStack,
// Frontend::start) or a server/ module.
int run_server(const std::string& config_path, bool check_only) {
  auto config = load_validated_config(config_path);
  if (!config) return 1;
  if (check_only) {
    // Also constructs the backends so per-backend keys ([export.local] identity, ...)
    // are validated exactly as a real startup would.
    auto exports = lnfs::core::ExportTable::build(std::move(*config));
    if (!exports) {
      LNFS_ERROR("invalid config {}: {}", config_path, lnfs::errno_name(exports.error()));
      return 1;
    }
    std::printf("configuration is valid\n");
    return 0;
  }
  const lnfs::core::ServerConfig server_cfg = config->server;
  apply_log_level(server_cfg);

  auto core = build_core_state(std::move(*config));
  if (!core) return 1;

  lnfs::init_async_logging({.file = server_cfg.log_file,
                            .rotate_size = server_cfg.log_rotate_size,
                            .rotate_keep = server_cfg.log_rotate_keep});

  lnfs::rt::Runtime runtime({.reactors = server_cfg.reactors,
                             .offload_threads = server_cfg.offload_threads,
                             .offload_heavy_threads = server_cfg.offload_heavy_threads,
                             .offload_queue_cap = server_cfg.offload_queue_cap,
                             .ring = server_cfg.ring,
                             .ring_sqpoll = server_cfg.ring_sqpoll});
  runtime.start();
  if (!start_backends(runtime, *core->exports)) {
    runtime.stop_and_join();
    return 1;
  }

  ProtocolStack stack(server_cfg, *core);
  if (server_cfg.enable_v4) stack.enable_v4(server_cfg, *core, runtime);
  // Prometheus text groups beyond the engines' own counters (server/metrics_providers).
  lnfs::server::register_metrics_providers(
      {.drc = stack.drc, .state = stack.state, .exports = *core->exports, .runtime = runtime});
  // Slow-request log + error-sampling ring sizing (plan doc 10 §3.6/§3.7).
  lnfs::obs::set_slow_request_threshold_us(
      static_cast<uint64_t>(server_cfg.slow_request_ms) * 1000);
  lnfs::obs::set_error_ring_capacity(server_cfg.error_ring);

  if (server_cfg.enable_v4 && stack.nfs4)
    stack.nfs4->configure_client_qos(server_cfg.client_read_bps,
                                     server_cfg.client_write_bps, server_cfg.client_iops);

  // Hot reload, step 1 (plan doc 10 §4.1): SIGHUP and `ctl reload` re-parse the config
  // file and apply the non-topology subset — log level, slow-request threshold, error
  // ring, per-export client allowlists + QoS, per-client QoS.  Everything else is
  // reported as restart-required.  Serialized: SIGHUP applies on the main wait loop,
  // ctl reload on the ctl reactor; both paths funnel through this one handler, and
  // concurrent invocations are harmless (last writer wins on independent knobs).
  auto do_reload = [config_path, server_cfg, &core, &stack]() -> std::string {
    auto fresh = load_validated_config(config_path);
    if (!fresh) return "reload failed: config invalid (details in the log)\n";
    std::string report;
    const auto& sc = fresh->server;
    if (sc.log_level != server_cfg.log_level) {
      apply_log_level(sc);
      report += std::format("log_level -> {}\n", sc.log_level);
    }
    lnfs::obs::set_slow_request_threshold_us(static_cast<uint64_t>(sc.slow_request_ms) *
                                             1000);
    lnfs::obs::set_error_ring_capacity(sc.error_ring);
    if (stack.nfs4)
      stack.nfs4->configure_client_qos(sc.client_read_bps, sc.client_write_bps,
                                       sc.client_iops);
    report += core->exports->reload_dynamic(*fresh);
    // Topology/runtime keys stay fixed until restart; call out what was ignored.
    if (sc.port != server_cfg.port || sc.mount_port != server_cfg.mount_port ||
        sc.bind != server_cfg.bind)
      report += "listen address/ports changed: restart required\n";
    if (sc.reactors != server_cfg.reactors ||
        sc.offload_threads != server_cfg.offload_threads)
      report += "thread topology changed: restart required\n";
    if (sc.state_dir != server_cfg.state_dir || sc.enable_v4 != server_cfg.enable_v4 ||
        sc.lease_seconds != server_cfg.lease_seconds ||
        sc.state_shards != server_cfg.state_shards)
      report += "state_dir/v4/lease/shards changed: restart required\n";
    if (sc.metrics_port != server_cfg.metrics_port ||
        sc.metrics_bind != server_cfg.metrics_bind)
      report += "metrics endpoint changed: restart required\n";
    LNFS_INFO("configuration reloaded from {}", config_path);
    return report.empty() ? "nothing to apply\n" : report;
  };

  auto frontend = lnfs::server::Frontend::start(server_cfg, runtime, stack, *core, do_reload);
  if (!frontend) {
    runtime.stop_and_join();
    return 1;
  }
  LNFS_INFO("lightnfs {} ready: nfs_port={} mount_port={} exports={}", LIGHTNFS_VERSION,
            frontend->nfs->port(), frontend->mount->port(),
            core->exports->entries().size());

  wait_for_shutdown_signal([&] {
    std::string report = do_reload();
    while (!report.empty() && report.back() == '\n') report.pop_back();
    std::replace(report.begin(), report.end(), '\n', ';');
    LNFS_INFO("reload (SIGHUP): {}", report);
  });

  stack.lease_stop.store(true);
  frontend->stop(server_cfg);
  stop_backends(runtime, *core->exports);
  runtime.stop_and_join();
  LNFS_INFO("lightnfs stopped");
  lnfs::shutdown_async_logging();
  return 0;
}

// ccmd callbacks return void; the exit code travels through this
// (0 success / 1 runtime failure / 2 usage error).
int g_exit = 0;

// cflag takes long-option values only as --name=value; fold the `--config FILE`
// form used by the acceptance scripts and the systemd unit into that shape.
std::vector<std::string> normalize_argv(int argc, char** argv) {
  std::vector<std::string> out;
  out.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    std::string a = argv[i];
    if ((a == "--config" || a == "-c") && i + 1 < argc) {
      out.push_back("--config=" + std::string(argv[++i]));
    } else {
      out.push_back(std::move(a));
    }
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  // Clients dictate creation modes over the wire; the server's own umask must not
  // subtract bits (the backend applies requested modes exactly).
  umask(0);

  auto root = std::make_shared<ccmd::c_command>(
      "lightnfsd", "lightnfsd --config=/etc/lightnfs/lightnfs.toml",
      "lightnfsd [--config=<path>] [--check-config]",
      "Userspace NFS gateway (NFSv3 + NFSv4.1/4.2). With no command the server runs "
      "until SIGINT/SIGTERM; --check-config validates the configuration and exits.",
      "userspace NFS gateway", [](const std::shared_ptr<ccmd::c_command>& c) {
        g_exit = run_server(c->var<std::string>("config"), c->var<bool>("check-config"));
      });
  root->varp<std::string>("config", "c", "/etc/lightnfs/lightnfs.toml",
                          "Path to the lightnfs TOML config file");
  root->var<bool>("check-config", false, "Validate the configuration and exit");

  try {
    root->execute(normalize_argv(argc, argv));
    return g_exit;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lightnfsd: %s\n", e.what());
    return 2;
  }
}
