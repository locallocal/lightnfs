#pragma once
// North side of lightnfsd (design 01 §1.4, 03 §3.1), in two parts (plan 10 A4):
//
//  - Management: the ctl unix socket and the optional Prometheus HTTP endpoint.  Lives
//    for the whole process — a standby gateway with no protocol stack still answers
//    `lightnfs-ctl status` and, later, `cluster takeover` — and owns the slot the ctl
//    commands read their data plane from.
//  - Frontend: the NFS and MOUNT listeners, rpcbind registration and the process-wide
//    RPC-over-TLS context.  Built after the protocol stack, torn down first on
//    shutdown; it attaches the data plane to the management side while it serves.
//
// ctl/metrics/rpcbind failures degrade with a warning; a listener or TLS failure
// aborts startup.

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

struct Management {
  std::unique_ptr<CtlServer> ctl;        // null when unavailable
  std::unique_ptr<MetricsHttp> metrics;  // null when disabled/unavailable
  // The data plane the ctl commands address: null until a frontend attaches one.
  std::shared_ptr<DataPlaneSlot> plane = std::make_shared<DataPlaneSlot>(nullptr);

  // Opens the ctl socket (with `reload` wired in and `role` feeding `status`) and
  // starts the metrics endpoint.  Needs the runtime; needs no protocol stack.
  static Management start(const core::ServerConfig& cfg, rt::Runtime& runtime,
                          std::function<std::string()> reload,
                          std::function<std::string()> role = {});
  // `plane` must stay valid until detach(); commands loaded before a detach may still
  // be using it, so the caller stops the ctl server before destroying what it points at.
  void attach(const DataPlane* plane) { this->plane->store(plane, std::memory_order_release); }
  void detach() { plane->store(nullptr, std::memory_order_release); }
  void stop();
};

struct Frontend {
  std::unique_ptr<transport::Listener> nfs, mount;
  // RPC-over-TLS (RFC 9289): process-global server context (null when tls = off); it
  // outlives every connection served by the listeners above.
  std::unique_ptr<transport::TlsContext> tls;
  // `ctl drain` state (plan doc 10 §4.2) and the data plane handed to Management:
  // heap-owned so Frontend stays movable while the ctl side keeps stable pointers.
  std::shared_ptr<std::atomic<bool>> draining = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<DataPlane> plane = std::make_shared<DataPlane>();

  // Builds the TLS context, creates and starts both listeners on `stack.dispatcher`,
  // registers the buffer-pool metric, attaches the data plane (exports, DRC, state,
  // drain) to `mgmt` and registers with rpcbind.
  static std::optional<Frontend> start(const core::ServerConfig& cfg, rt::Runtime& runtime,
                                       ProtocolStack& stack, CoreState& core,
                                       Management& mgmt);
  // Detaches the data plane, stops the accept loops and unregisters from rpcbind.
  // Established connections drain with the runtime.
  void stop(const core::ServerConfig& cfg, Management& mgmt);
};

}  // namespace lnfs::server
