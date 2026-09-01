#include <sys/stat.h>
#include <unistd.h>

#include <ccmd.h>

#include <algorithm>
#include <array>
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

#include "backend/local.hpp"
#include "core/boot_epoch.hpp"
#include "core/config.hpp"
#include "core/file_handle.hpp"
#include "core/obj_lock.hpp"
#include "mountd/mount3.hpp"
#include "core/pseudofs.hpp"
#include "nfsv3/engine.hpp"
#include "nfsv4/engine.hpp"
#include "state/state_mgr.hpp"
#include "obs/errlog.hpp"
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
        pseudofs(*core.exports, core.epoch),
        state({.boot_epoch = core.epoch,
               .state_dir = cfg.state_dir,
               .lease_seconds = cfg.lease_seconds,
               .grace_seconds = cfg.grace_seconds,
               .courtesy_multiplier = cfg.courtesy_multiplier,
               .max_io = cfg.max_request_size,
               .shards = cfg.state_shards,
               .delegations = cfg.delegations}) {
    nfs3.set_write_verifier(lnfs::core::verifier_from_epoch(core.epoch));
    nfs3.set_drc(&drc);
    nfs3.register_with(dispatcher);
    mount.register_with(dispatcher);
  }

  // Grace list + COMPOUND engine + the lease scanner coroutine (07 §7.4: expiry →
  // courtesy → conflict/timeout reclaim) on reactor 0.
  void enable_v4(const lnfs::core::ServerConfig& cfg, CoreState& core,
                 lnfs::rt::Runtime& runtime) {
    state.load_grace_list();
    // RFC 8881 §2.10.4 identity: default derives from hostname + state_dir so two
    // distinct lightnfs instances never look like trunking paths of one server.
    char host[256] = "lightnfs";
    (void)::gethostname(host, sizeof host - 1);
    std::string derived = std::string(host) + ":" + cfg.state_dir;
    nfs4.emplace(*core.exports, core.key, locks, pseudofs, state,
                 cfg.server_owner.empty() ? derived : cfg.server_owner,
                 cfg.server_scope.empty() ? derived : cfg.server_scope);
    nfs4->register_with(dispatcher);
    // Off reactor 0 (plan doc 10 §2.6): the auxiliary tasks used to pile onto the
    // same reactor the (old, single) accept loop lived on.
    lnfs::rt::spawn(state.run_lease_scanner(&lease_stop),
                    runtime.reactor(runtime.reactor_count() - 1));
  }
};

// Prometheus text groups for the DRC, the v4 state tables, per-export data-path
// counters, and the local-backend fd caches (design 08 §8.3, plan doc 10 §3).
void register_metrics_providers(ProtocolStack& stack, lnfs::core::ExportTable& exports) {
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
        "lightnfs_v4_share_denied_total {}\nlightnfs_v4_open_merges_total {}\n"
        // Lock-state gauges promised by deployment.md (plan doc 10 §3.4).
        "lightnfs_v4_lock_states {}\nlightnfs_v4_lock_segments {}\n"
        "lightnfs_v4_lock_owners {}\nlightnfs_v4_lock_denied_total {}\n"
        // Delegations + backchannel (plan doc 10 §5.2).
        "lightnfs_v4_delegations {}\nlightnfs_v4_deleg_grants_total {}\n"
        "lightnfs_v4_deleg_recalls_total {}\nlightnfs_v4_deleg_returns_total {}\n"
        "lightnfs_v4_deleg_revokes_total {}\nlightnfs_v4_cb_lock_notifies_total {}\n",
        s.clients, s.sessions, s.opens, s.seq_new, s.seq_replay, s.seq_misordered,
        s.seq_waits, s.grace ? 1 : 0, s.grace_remaining, s.files, s.courtesy,
        s.lease_expirations, s.reclaim_conflict, s.reclaim_timeout, s.reclaim_forced,
        s.share_denied, s.open_merges, s.lock_states, s.lock_segments, s.lock_owners,
        s.lock_denied, s.delegs, s.deleg_grants, s.deleg_recalls, s.deleg_returns,
        s.deleg_revokes, s.cb_lock_notifies);
  });
  lnfs::obs::register_text_provider([&exports](std::string& out) {
    for (const auto& entry : exports.entries()) {
      std::string labels =
          std::format("export=\"{}\",fsid=\"{}\"", entry->path, entry->fsid);
      const auto& em = entry->metrics;
      out += std::format(
          "lightnfs_export_read_bytes_total{{{0}}} {1}\n"
          "lightnfs_export_write_bytes_total{{{0}}} {2}\n"
          "lightnfs_export_read_ops_total{{{0}}} {3}\n"
          "lightnfs_export_write_ops_total{{{0}}} {4}\n",
          labels, em.read_bytes.load(), em.write_bytes.load(), em.read_ops.load(),
          em.write_ops.load());
      // Local-backend fd/resolve cache counters (previously ctl text only, §3.5).
      auto* local = dynamic_cast<lnfs::backend::LocalBackend*>(entry->backend.get());
      if (!local) continue;
      auto s = local->fd_cache_stats();
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
  });
}

// North side: NFS/MOUNT listeners, ctl socket, optional metrics HTTP, rpcbind
// registration. ctl/metrics failures degrade with a warning; listener failure aborts.
struct Frontend {
  std::unique_ptr<lnfs::transport::Listener> nfs, mount;
  std::unique_ptr<lnfs::server::CtlServer> ctl;        // null when unavailable
  std::unique_ptr<lnfs::server::MetricsHttp> metrics;  // null when disabled/unavailable
  // RPC-over-TLS (RFC 9289): process-global server context (null when tls = off); it
  // outlives every connection served by the listeners below.
  std::unique_ptr<lnfs::transport::TlsContext> tls;
  // `ctl drain` state (plan doc 10 §4.2): heap-owned so Frontend stays movable while
  // CtlDeps keeps a stable pointer.
  std::shared_ptr<std::atomic<bool>> draining = std::make_shared<std::atomic<bool>>(false);
};

std::optional<Frontend> start_frontend(const lnfs::core::ServerConfig& cfg,
                                       lnfs::rt::Runtime& runtime, ProtocolStack& stack,
                                       CoreState& core,
                                       std::function<std::string()> reload) {
  lnfs::transport::TransportConfig transport_cfg;
  transport_cfg.max_request_size = cfg.max_request_size;
  transport_cfg.max_inflight_per_conn = cfg.inflight_per_conn;
  transport_cfg.max_connections = cfg.max_connections;
  transport_cfg.per_peer_limit = cfg.per_peer_limit;

  // RPC-over-TLS (RFC 9289, plan doc 10 §5.4): build the server context before the
  // listeners so both the NFS and MOUNT ports offer STARTTLS.  A cert/key error aborts
  // startup (config validation already checked the mode and file presence).
  std::unique_ptr<lnfs::transport::TlsContext> tls_ctx;
  if (cfg.tls_mode != "off") {
    lnfs::transport::TlsConfig tls_cfg;
    tls_cfg.policy = cfg.tls_mode == "required" ? lnfs::transport::TlsPolicy::kRequired
                                                : lnfs::transport::TlsPolicy::kOptional;
    tls_cfg.cert = cfg.tls_cert;
    tls_cfg.key = cfg.tls_key;
    tls_cfg.ca = cfg.tls_ca;
    tls_cfg.require_client_cert = cfg.tls_require_client_cert;
    auto ctx = lnfs::transport::TlsContext::create(tls_cfg);
    if (!ctx) {
      LNFS_ERROR("cannot initialize TLS ({} mode): {}", cfg.tls_mode,
                 lnfs::errno_name(ctx.error()));
      return std::nullopt;
    }
    tls_ctx = std::move(*ctx);
    transport_cfg.tls = tls_ctx.get();
    transport_cfg.tls_policy = tls_cfg.policy;
    LNFS_INFO("RPC-over-TLS enabled (mode={}, mutual={})", cfg.tls_mode,
              cfg.tls_require_client_cert);
  }

  auto nfs_listener = lnfs::transport::Listener::create(cfg.port, transport_cfg,
                                                        stack.dispatcher, runtime, cfg.bind);
  auto mount_listener = lnfs::transport::Listener::create(
      cfg.mount_port, transport_cfg, stack.dispatcher, runtime, cfg.bind);
  if (!nfs_listener || !mount_listener) {
    LNFS_ERROR("cannot create listeners: nfs={} mount={}",
               nfs_listener ? "ok" : lnfs::errno_name(nfs_listener.error()),
               mount_listener ? "ok" : lnfs::errno_name(mount_listener.error()));
    return std::nullopt;
  }
  Frontend fe{std::move(*nfs_listener), std::move(*mount_listener), nullptr, nullptr,
              std::move(tls_ctx)};
  fe.nfs->start();    // per-reactor REUSEPORT accept loops (plan doc 10 §2.3)
  fe.mount->start();
  // Buffer-pool watermark (plan doc 10 §3.5); the listeners are heap-allocated and
  // outlive every metrics scrape (frontend stops before run_server returns).
  lnfs::obs::register_text_provider(
      [nfs = fe.nfs.get(), mount = fe.mount.get()](std::string& out) {
        out += std::format(
            "lightnfs_buffer_pool_free_bytes{{listener=\"nfs\"}} {}\n"
            "lightnfs_buffer_pool_free_bytes{{listener=\"mount\"}} {}\n",
            nfs->pool().free_bytes(), mount->pool().free_bytes());
      });

  std::string ctl_path =
      cfg.ctl_socket.empty() ? cfg.state_dir + "/ctl.sock" : cfg.ctl_socket;
  // `drain` stops the accept loops but keeps serving established connections — the
  // graceful way off a load balancer (plan doc 10 §4.2).  Irreversible until restart.
  auto drain = [nfs = fe.nfs.get(), mount = fe.mount.get(),
                draining = fe.draining]() -> std::string {
    if (draining->exchange(true, std::memory_order_relaxed))
      return "already draining\n";
    nfs->request_stop();
    mount->request_stop();
    LNFS_INFO("draining: accept loops stopped, serving existing connections only");
    return "draining: no new connections will be accepted\n";
  };
  auto ctl = lnfs::server::CtlServer::create(
      ctl_path, {.exports = core.exports.get(),
                 .drc = &stack.drc,
                 .state = &stack.state,
                 .reload = std::move(reload),
                 .drain = std::move(drain),
                 .draining = fe.draining.get(),
                 .started = std::chrono::steady_clock::now()});
  if (ctl) {
    fe.ctl = std::move(*ctl);
    lnfs::rt::spawn(fe.ctl->run(), runtime.reactor(1 % runtime.reactor_count()));
  } else {
    LNFS_WARN("ctl socket unavailable at {}: {}", ctl_path, lnfs::errno_name(ctl.error()));
  }

  if (cfg.metrics_port != 0) {
    std::vector<lnfs::core::Cidr> allow;
    for (const auto& text : cfg.metrics_allow) {
      auto cidr = lnfs::core::Cidr::parse(text);  // validated at config load
      if (cidr) allow.push_back(std::move(*cidr));
    }
    auto metrics = lnfs::server::MetricsHttp::create(cfg.metrics_port, cfg.metrics_bind,
                                                     std::move(allow));
    if (metrics) {
      fe.metrics = std::move(*metrics);
      lnfs::rt::spawn(fe.metrics->run(), runtime.reactor(2 % runtime.reactor_count()));
    } else {
      LNFS_WARN("metrics endpoint {}:{} unavailable", cfg.metrics_bind, cfg.metrics_port);
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
  register_metrics_providers(stack, *core->exports);
  // Slow-request log + error-sampling ring sizing (plan doc 10 §3.6/§3.7).
  lnfs::obs::set_slow_request_threshold_us(
      static_cast<uint64_t>(server_cfg.slow_request_ms) * 1000);
  lnfs::obs::set_error_ring_capacity(server_cfg.error_ring);
  // Runtime-layer metrics (plan doc 10 §2.5/§3.5, design 08 §8.3): offload queue depth,
  // per-class throughput, and the reactor loop busy-period histogram.
  lnfs::obs::register_text_provider([&runtime](std::string& out) {
    auto s = runtime.offload().stats();
    static constexpr const char* kCls[] = {"light", "heavy"};
    for (int c = 0; c < lnfs::rt::kOffloadClasses; ++c) {
      out += std::format(
          "lightnfs_offload_queue_depth{{class=\"{}\"}} {}\n"
          "lightnfs_offload_submitted_total{{class=\"{}\"}} {}\n"
          "lightnfs_offload_completed_total{{class=\"{}\"}} {}\n"
          "lightnfs_offload_admission_deferred_total{{class=\"{}\"}} {}\n",
          kCls[c], s.depth[c], kCls[c], s.submitted[c], kCls[c], s.completed[c],
          kCls[c], s.deferred[c]);
    }
    std::array<uint64_t, lnfs::rt::Reactor::kLoopBuckets> agg{};
    uint64_t sum_us = 0;
    for (size_t i = 0; i < runtime.reactor_count(); ++i) {
      auto ls = runtime.reactor(i).loop_stats();
      for (size_t b = 0; b < agg.size(); ++b) agg[b] += ls.buckets[b];
      sum_us += ls.sum_us;
    }
    out += "# TYPE lightnfs_reactor_loop_duration_seconds histogram\n";
    lnfs::obs::append_histogram(out, "lightnfs_reactor_loop_duration_seconds", "",
                                lnfs::rt::Reactor::kLoopBoundsUs, agg, sum_us);
  });

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

  auto frontend = start_frontend(server_cfg, runtime, stack, *core, do_reload);
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
