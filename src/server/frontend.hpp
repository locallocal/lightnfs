#pragma once
// North side of lightnfsd (design 01 §1.4, 03 §3.1): the NFS and MOUNT listeners, the
// ctl unix socket, the optional Prometheus HTTP endpoint, rpcbind registration and the
// process-wide RPC-over-TLS context.  Built after the protocol stack, torn down first
// on shutdown.  ctl/metrics/rpcbind failures degrade with a warning; a listener or
// TLS failure aborts startup.

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "core/config.hpp"
#include "runtime/runtime.hpp"
#include "server/ctl.hpp"
#include "server/protocol_stack.hpp"
#include "transport/listener.hpp"

namespace lnfs::server {

struct Frontend {
  std::unique_ptr<transport::Listener> nfs, mount;
  std::unique_ptr<CtlServer> ctl;        // null when unavailable
  std::unique_ptr<MetricsHttp> metrics;  // null when disabled/unavailable
  // RPC-over-TLS (RFC 9289): process-global server context (null when tls = off); it
  // outlives every connection served by the listeners above.
  std::unique_ptr<transport::TlsContext> tls;
  // `ctl drain` state (plan doc 10 §4.2): heap-owned so Frontend stays movable while
  // CtlDeps keeps a stable pointer.
  std::shared_ptr<std::atomic<bool>> draining = std::make_shared<std::atomic<bool>>(false);

  // Builds the TLS context, creates and starts both listeners on `stack.dispatcher`,
  // registers the buffer-pool metric, opens the ctl socket (with `reload` and `drain`
  // wired in), starts the metrics endpoint and registers with rpcbind.
  static std::optional<Frontend> start(const core::ServerConfig& cfg, rt::Runtime& runtime,
                                       ProtocolStack& stack, CoreState& core,
                                       std::function<std::string()> reload);
  // Stops accept loops, ctl and metrics, and unregisters from rpcbind.  Established
  // connections drain with the runtime.
  void stop(const core::ServerConfig& cfg);
};

}  // namespace lnfs::server
