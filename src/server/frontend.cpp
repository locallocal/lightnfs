#include "server/frontend.hpp"

#include <chrono>
#include <format>
#include <thread>
#include <utility>
#include <vector>

#include "mountd/mount3.hpp"
#include "nfsv3/engine.hpp"
#include "obs/metrics.hpp"
#include "server/rpcbind.hpp"
#include "transport/tls.hpp"
#include "util/log.hpp"

namespace lnfs::server {

Management Management::start(const core::ServerConfig& cfg, rt::Runtime& runtime,
                             std::function<std::string()> reload,
                             std::function<std::string()> role) {
  Management mgmt;
  std::string ctl_path = cfg.ctl_socket.empty() ? cfg.state_dir + "/ctl.sock" : cfg.ctl_socket;
  auto ctl = CtlServer::create(ctl_path, {.reload = std::move(reload),
                                          .started = std::chrono::steady_clock::now(),
                                          .plane = mgmt.plane,
                                          .role = std::move(role)});
  if (ctl) {
    mgmt.ctl = std::move(*ctl);
    mgmt.ctl->start(runtime.reactor(1 % runtime.reactor_count()));
  } else {
    LNFS_WARN("ctl socket unavailable at {}: {}", ctl_path, errno_name(ctl.error()));
  }

  if (cfg.metrics_port != 0) {
    std::vector<core::Cidr> allow;
    for (const auto& text : cfg.metrics_allow) {
      auto cidr = core::Cidr::parse(text);  // validated at config load
      if (cidr) allow.push_back(std::move(*cidr));
    }
    auto metrics = MetricsHttp::create(cfg.metrics_port, cfg.metrics_bind, std::move(allow));
    if (metrics) {
      mgmt.metrics = std::move(*metrics);
      mgmt.metrics->start(runtime.reactor(2 % runtime.reactor_count()));
    } else {
      LNFS_WARN("metrics endpoint {}:{} unavailable", cfg.metrics_bind, cfg.metrics_port);
    }
  }
  return mgmt;
}

void Management::detach() {
  if (plane->load() == nullptr && plane->pins() == 0) return;
  if (!plane->detach(std::chrono::seconds(5)))
    LNFS_WARN("ctl: {} command(s) still using the data plane after detach; continuing",
              plane->pins());
}

void Management::stop() {
  detach();
  if (ctl) ctl->request_stop();
  if (metrics) metrics->request_stop();
  // Join both accept loops: after this the servers may be destroyed with the
  // runtime still running (a stopped ctl loop must not keep accepting on a closed fd).
  if (ctl) ctl->wait_stopped();
  if (metrics) metrics->wait_stopped();
}

std::optional<Frontend> Frontend::start(const core::ServerConfig& cfg, rt::Runtime& runtime,
                                        ProtocolStack& stack, CoreState& core,
                                        Management& mgmt) {
  transport::TransportConfig transport_cfg;
  transport_cfg.max_request_size = cfg.max_request_size;
  transport_cfg.max_inflight_per_conn = cfg.inflight_per_conn;
  transport_cfg.max_connections = cfg.max_connections;
  transport_cfg.per_peer_limit = cfg.per_peer_limit;

  // RPC-over-TLS (RFC 9289, plan doc 10 §5.4): build the server context before the
  // listeners so both the NFS and MOUNT ports offer STARTTLS.  A cert/key error aborts
  // startup (config validation already checked the mode and file presence).
  std::unique_ptr<transport::TlsContext> tls_ctx;
  if (cfg.tls_mode != "off") {
    transport::TlsConfig tls_cfg;
    tls_cfg.policy = cfg.tls_mode == "required" ? transport::TlsPolicy::kRequired
                                                : transport::TlsPolicy::kOptional;
    tls_cfg.cert = cfg.tls_cert;
    tls_cfg.key = cfg.tls_key;
    tls_cfg.ca = cfg.tls_ca;
    tls_cfg.require_client_cert = cfg.tls_require_client_cert;
    auto ctx = transport::TlsContext::create(tls_cfg);
    if (!ctx) {
      LNFS_ERROR("cannot initialize TLS ({} mode): {}", cfg.tls_mode, errno_name(ctx.error()));
      return std::nullopt;
    }
    tls_ctx = std::move(*ctx);
    transport_cfg.tls = tls_ctx.get();
    transport_cfg.tls_policy = tls_cfg.policy;
    LNFS_INFO("RPC-over-TLS enabled (mode={}, mutual={})", cfg.tls_mode,
              cfg.tls_require_client_cert);
  }

  auto nfs_listener =
      transport::Listener::create(cfg.port, transport_cfg, stack.dispatcher, runtime, cfg.bind);
  auto mount_listener = transport::Listener::create(cfg.mount_port, transport_cfg,
                                                    stack.dispatcher, runtime, cfg.bind);
  if (!nfs_listener || !mount_listener) {
    LNFS_ERROR("cannot create listeners: nfs={} mount={}",
               nfs_listener ? "ok" : errno_name(nfs_listener.error()),
               mount_listener ? "ok" : errno_name(mount_listener.error()));
    return std::nullopt;
  }
  Frontend fe{std::move(*nfs_listener), std::move(*mount_listener), std::move(tls_ctx)};
  fe.nfs->start();  // per-reactor REUSEPORT accept loops (plan doc 10 §2.3)
  fe.mount->start();
  // Buffer-pool watermark (plan doc 10 §3.5); the listeners are heap-allocated and
  // outlive every metrics scrape (the frontend stops before run_server returns).
  fe.pool_metric = obs::register_text_provider(
      [nfs = fe.nfs.get(), mount = fe.mount.get()](std::string& out) {
        out += std::format(
            "lightnfs_buffer_pool_free_bytes{{listener=\"nfs\"}} {}\n"
            "lightnfs_buffer_pool_free_bytes{{listener=\"mount\"}} {}\n",
            nfs->pool().free_bytes(), mount->pool().free_bytes());
      });

  // `drain` stops the accept loops but keeps serving established connections — the
  // graceful way off a load balancer (plan doc 10 §4.2).  Irreversible until restart.
  auto drain = [nfs = fe.nfs.get(), mount = fe.mount.get(),
                draining = fe.draining]() -> std::string {
    if (draining->exchange(true, std::memory_order_relaxed)) return "already draining\n";
    nfs->request_stop();
    mount->request_stop();
    LNFS_INFO("draining: accept loops stopped, serving existing connections only");
    return "draining: no new connections will be accepted\n";
  };
  *fe.plane = DataPlane{.exports = core.exports.get(),
                        .drc = &stack.drc,
                        .state = &stack.state,
                        .drain = std::move(drain),
                        .draining = fe.draining.get()};
  mgmt.attach(fe.plane.get());

  if (cfg.rpcbind) {
    auto nfs_reg = rpcbind_set(nfsv3::kProgram, nfsv3::kVersion, fe.nfs->port());
    auto mount_reg = rpcbind_set(mountd::kProgram, mountd::kVersion, fe.mount->port());
    if (!nfs_reg || !mount_reg)
      LNFS_WARN("rpcbind registration unavailable; use explicit port/mountport options");
  }
  return fe;
}

void Frontend::stop(const core::ServerConfig& cfg, Management& mgmt) {
  mgmt.detach();
  nfs->request_stop();
  mount->request_stop();
  nfs->wait_stopped();
  mount->wait_stopped();
  if (pool_metric) {
    obs::unregister_text_provider(pool_metric);
    pool_metric = 0;
  }
  if (cfg.rpcbind) {
    (void)rpcbind_unset(nfsv3::kProgram, nfsv3::kVersion);
    (void)rpcbind_unset(mountd::kProgram, mountd::kVersion);
  }
}

bool Frontend::drain_connections(std::chrono::milliseconds grace,
                                 std::chrono::milliseconds close_timeout) {
  auto& registry = transport::ConnRegistry::instance();
  if (!registry.wait_idle(grace)) {
    size_t closed = registry.close_all();
    LNFS_INFO("data plane: {} connection(s) still open after {} ms grace, shutting them down",
              closed, grace.count());
    if (!registry.wait_idle(close_timeout)) {
      LNFS_WARN("data plane: {} connection(s) did not close within {} ms", registry.count(),
                close_timeout.count());
      return false;
    }
  }
  // A connection leaves the registry just before its last touch of the listener's
  // tracker: wait for that too.
  auto deadline = std::chrono::steady_clock::now() + close_timeout;
  while (nfs->tracker().count() > 0 || mount->tracker().count() > 0) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

}  // namespace lnfs::server
