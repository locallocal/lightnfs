#include <sys/stat.h>

#include <ccmd.h>

#include <atomic>
#include <chrono>
#include <csignal>
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
#include "core/file_handle.hpp"
#include "core/obj_lock.hpp"
#include "mountd/mount3.hpp"
#include "core/pseudofs.hpp"
#include "nfsv3/engine.hpp"
#include "nfsv4/engine.hpp"
#include "state/state_mgr.hpp"
#include "obs/metrics.hpp"
#include "rpc/drc.hpp"
#include "runtime/runtime.hpp"
#include "server/ctl.hpp"
#include "server/rpcbind.hpp"
#include "transport/listener.hpp"
#include "util/log.hpp"

namespace {

volatile std::sig_atomic_t stopping = 0;
void on_signal(int) { stopping = 1; }

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

// ---- run_server phases -----------------------------------------------------
// Startup order (config → identity → backends → engines → listeners) and the
// mirror-image shutdown live in run_server; each phase below is one self-contained
// step of it.

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

// Durable identity: export table, handle HMAC key (bound to the exports), boot epoch.
struct CoreState {
  std::unique_ptr<lnfs::core::ExportTable> exports;
  lnfs::core::FileHandleCodec key;
  uint64_t epoch = 0;
};

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
  }
  return true;
}

void stop_backends(lnfs::rt::Runtime& runtime, lnfs::core::ExportTable& exports) {
  for (const auto& entry : exports.entries())
    (void)run_backend_hook(runtime.reactor(0), entry->backend->stop());
}

// Protocol engines and their shared state, wired onto one dispatcher. Members are
// declared in dependency order; everything lives until run_server returns.
struct ProtocolStack {
  lnfs::rpc::Dispatcher dispatcher;
  lnfs::core::ObjLockRegistry locks;
  lnfs::rpc::Drc drc;
  lnfs::nfsv3::Engine nfs3;
  lnfs::mountd::Mount3 mount;
  // v4.1 stack (phase 3): pseudo-fs namespace + session state + COMPOUND engine.
  lnfs::core::PseudoFs pseudofs;
  lnfs::state::StateMgr state;
  std::optional<lnfs::nfsv4::Engine> nfs4;
  std::atomic<bool> lease_stop{false};

  ProtocolStack(const lnfs::core::ServerConfig& cfg, CoreState& core)
      : drc({.ttl = std::chrono::milliseconds(cfg.drc_ttl_ms), .max_memory = cfg.drc_mem}),
        nfs3(*core.exports, core.key, locks),
        mount(*core.exports, core.key),
        pseudofs(*core.exports),
        state({.boot_epoch = core.epoch,
               .state_dir = cfg.state_dir,
               .lease_seconds = cfg.lease_seconds,
               .courtesy_multiplier = cfg.courtesy_multiplier,
               .max_io = cfg.max_request_size}) {
    nfs3.set_write_verifier(lnfs::core::verifier_from_epoch(core.epoch));
    nfs3.set_drc(&drc);
    nfs3.register_with(dispatcher);
    mount.register_with(dispatcher);
  }

  // Grace list + COMPOUND engine + the lease scanner coroutine (07 §7.4: expiry →
  // courtesy → conflict/timeout reclaim) on reactor 0.
  void enable_v4(CoreState& core, lnfs::rt::Runtime& runtime) {
    state.load_grace_list();
    nfs4.emplace(*core.exports, core.key, locks, pseudofs, state);
    nfs4->register_with(dispatcher);
    lnfs::rt::spawn(state.run_lease_scanner(&lease_stop), runtime.reactor(0));
  }
};

// Prometheus text groups for the DRC and the v4 state tables (design 08 §8.3).
void register_metrics_providers(ProtocolStack& stack) {
  lnfs::obs::register_text_provider([&drc = stack.drc](std::string& out) {
    auto s = drc.stats();
    out += std::format(
        "lightnfs_drc_inserts_total {}\nlightnfs_drc_replays_total {}\n"
        "lightnfs_drc_waits_total {}\nlightnfs_drc_evictions_total {}\n"
        "lightnfs_drc_entries {}\nlightnfs_drc_bytes {}\n",
        s.inserts, s.replays, s.waits, s.evictions, s.entries, s.bytes);
  });
  lnfs::obs::register_text_provider([&state = stack.state](std::string& out) {
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
        "lightnfs_v4_share_denied_total {}\nlightnfs_v4_open_merges_total {}\n",
        s.clients, s.sessions, s.opens, s.seq_new, s.seq_replay, s.seq_misordered,
        s.seq_waits, s.grace ? 1 : 0, s.grace_remaining, s.files, s.courtesy,
        s.lease_expirations, s.reclaim_conflict, s.reclaim_timeout, s.reclaim_forced,
        s.share_denied, s.open_merges);
  });
}

// North side: NFS/MOUNT listeners, ctl socket, optional metrics HTTP, rpcbind
// registration. ctl/metrics failures degrade with a warning; listener failure aborts.
struct Frontend {
  std::unique_ptr<lnfs::transport::Listener> nfs, mount;
  std::unique_ptr<lnfs::server::CtlServer> ctl;        // null when unavailable
  std::unique_ptr<lnfs::server::MetricsHttp> metrics;  // null when disabled/unavailable
};

std::optional<Frontend> start_frontend(const lnfs::core::ServerConfig& cfg,
                                       lnfs::rt::Runtime& runtime, ProtocolStack& stack,
                                       CoreState& core) {
  lnfs::transport::TransportConfig transport_cfg;
  transport_cfg.max_request_size = cfg.max_request_size;
  transport_cfg.max_inflight_per_conn = cfg.inflight_per_conn;
  transport_cfg.max_connections = cfg.max_connections;
  transport_cfg.per_peer_limit = cfg.per_peer_limit;
  auto nfs_listener =
      lnfs::transport::Listener::create(cfg.port, transport_cfg, stack.dispatcher, runtime);
  auto mount_listener = lnfs::transport::Listener::create(cfg.mount_port, transport_cfg,
                                                          stack.dispatcher, runtime);
  if (!nfs_listener || !mount_listener) {
    LNFS_ERROR("cannot create listeners: nfs={} mount={}",
               nfs_listener ? "ok" : lnfs::errno_name(nfs_listener.error()),
               mount_listener ? "ok" : lnfs::errno_name(mount_listener.error()));
    return std::nullopt;
  }
  Frontend fe{std::move(*nfs_listener), std::move(*mount_listener), nullptr, nullptr};
  lnfs::rt::spawn(fe.nfs->run(), runtime.reactor(0));
  lnfs::rt::spawn(fe.mount->run(), runtime.reactor(0));

  std::string ctl_path =
      cfg.ctl_socket.empty() ? cfg.state_dir + "/ctl.sock" : cfg.ctl_socket;
  auto ctl = lnfs::server::CtlServer::create(
      ctl_path, {.exports = core.exports.get(), .drc = &stack.drc, .state = &stack.state});
  if (ctl) {
    fe.ctl = std::move(*ctl);
    lnfs::rt::spawn(fe.ctl->run(), runtime.reactor(0));
  } else {
    LNFS_WARN("ctl socket unavailable at {}: {}", ctl_path, lnfs::errno_name(ctl.error()));
  }

  if (cfg.metrics_port != 0) {
    auto metrics = lnfs::server::MetricsHttp::create(cfg.metrics_port);
    if (metrics) {
      fe.metrics = std::move(*metrics);
      lnfs::rt::spawn(fe.metrics->run(), runtime.reactor(0));
    } else {
      LNFS_WARN("metrics port {} unavailable", cfg.metrics_port);
    }
  }

  if (cfg.rpcbind) {
    auto nfs_reg = lnfs::server::rpcbind_set(lnfs::nfsv3::kProgram, lnfs::nfsv3::kVersion,
                                             fe.nfs->port());
    auto mount_reg = lnfs::server::rpcbind_set(lnfs::mountd::kProgram,
                                               lnfs::mountd::kVersion, fe.mount->port());
    if (!nfs_reg || !mount_reg)
      LNFS_WARN("rpcbind registration unavailable; use explicit port/mountport options");
  }
  return fe;
}

void stop_frontend(const lnfs::core::ServerConfig& cfg, Frontend& fe) {
  fe.nfs->request_stop();
  fe.mount->request_stop();
  if (fe.ctl) fe.ctl->request_stop();
  if (fe.metrics) fe.metrics->request_stop();
  if (cfg.rpcbind) {
    (void)lnfs::server::rpcbind_unset(lnfs::nfsv3::kProgram, lnfs::nfsv3::kVersion);
    (void)lnfs::server::rpcbind_unset(lnfs::mountd::kProgram, lnfs::mountd::kVersion);
  }
}

void wait_for_shutdown_signal() {
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

int run_server(const std::string& config_path, bool check_only) {
  auto config = load_validated_config(config_path);
  if (!config) return 1;
  if (check_only) {
    std::printf("configuration is valid\n");
    return 0;
  }
  const lnfs::core::ServerConfig server_cfg = config->server;
  apply_log_level(server_cfg);

  auto core = build_core_state(std::move(*config));
  if (!core) return 1;

  lnfs::init_async_logging();

  lnfs::rt::Runtime runtime({.reactors = server_cfg.reactors,
                             .offload_threads = server_cfg.offload_threads});
  runtime.start();
  if (!start_backends(runtime, *core->exports)) {
    runtime.stop_and_join();
    return 1;
  }

  ProtocolStack stack(server_cfg, *core);
  if (server_cfg.enable_v4) stack.enable_v4(*core, runtime);
  register_metrics_providers(stack);

  auto frontend = start_frontend(server_cfg, runtime, stack, *core);
  if (!frontend) {
    runtime.stop_and_join();
    return 1;
  }
  LNFS_INFO("lightnfs ready: nfs_port={} mount_port={} exports={}", frontend->nfs->port(),
            frontend->mount->port(), core->exports->entries().size());

  wait_for_shutdown_signal();

  stack.lease_stop.store(true);
  stop_frontend(server_cfg, *frontend);
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
