#pragma once
// The rebuildable data plane (plan 10 C1, design 09 §9.6): the protocol stack, the
// listeners in front of it and the metrics providers reading it, built as one unit
// by activate() and torn down by deactivate() while the runtime, the backends and the
// management plane keep running.  A single gateway activates once at startup and
// deactivates at exit; the cluster controller (plan 10 C2) does it on every takeover,
// because the stack fixes the epoch (write verifier, stateid epoch, server identity)
// when it is constructed.

#include <chrono>
#include <memory>
#include <optional>

#include "core/config.hpp"
#include "runtime/runtime.hpp"
#include "server/frontend.hpp"
#include "server/metrics_providers.hpp"
#include "server/protocol_stack.hpp"

namespace lnfs::server {

struct DataPlaneInstance {
  std::unique_ptr<ProtocolStack> stack;
  std::optional<Frontend> frontend;
  MetricsRegistration metrics;
};

// Builds the stack (v4 when enabled: grace list, engine, lease scanner), registers
// the metrics providers, applies the QoS knobs and starts the frontend, which attaches
// the data plane to `mgmt`.  nullopt (reason logged) leaves nothing behind.
std::optional<DataPlaneInstance> activate(const core::ServerConfig& cfg,
                                          const core::ClusterConfig& cluster, CoreState& core,
                                          rt::Runtime& runtime, Management& mgmt);

// Mirror image: detach from ctl and stop accepting → let connections finish for
// `grace`, then close the rest → join the lease scanner → drop the providers → destroy
// the frontend, then the stack.  False when connections did not converge in time (the
// instance is then still destroyed; this is the signal to log and count).
bool deactivate(DataPlaneInstance& instance, const core::ServerConfig& cfg, Management& mgmt,
                std::chrono::milliseconds grace,
                std::chrono::milliseconds close_timeout = std::chrono::seconds(5));

}  // namespace lnfs::server
