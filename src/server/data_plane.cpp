#include "server/data_plane.hpp"

#include "obs/errlog.hpp"
#include "util/log.hpp"

namespace lnfs::server {

std::optional<DataPlaneInstance> activate(const core::ServerConfig& cfg,
                                          const core::ClusterConfig& cluster, CoreState& core,
                                          rt::Runtime& runtime, Management& mgmt) {
  DataPlaneInstance inst;
  inst.stack = std::make_unique<ProtocolStack>(cfg, core);
  if (cfg.enable_v4) inst.stack->enable_v4(cfg, cluster, core, runtime);
  inst.metrics = register_metrics_providers({.drc = inst.stack->drc,
                                             .state = inst.stack->state,
                                             .exports = *core.exports,
                                             .runtime = runtime});
  if (inst.stack->nfs4)
    inst.stack->nfs4->configure_client_qos(cfg.client_read_bps, cfg.client_write_bps,
                                           cfg.client_iops);
  inst.frontend = Frontend::start(cfg, runtime, *inst.stack, core, mgmt);
  if (!inst.frontend) {
    inst.metrics.reset();
    inst.stack->stop_lease_scanner();
    return std::nullopt;
  }
  return inst;
}

bool deactivate(DataPlaneInstance& instance, const core::ServerConfig& cfg, Management& mgmt,
                std::chrono::milliseconds grace, std::chrono::milliseconds close_timeout) {
  bool converged = true;
  if (instance.frontend) {
    instance.frontend->stop(cfg, mgmt);
    converged = instance.frontend->drain_connections(grace, close_timeout);
  }
  if (instance.stack) instance.stack->stop_lease_scanner();
  instance.metrics.reset();
  instance.frontend.reset();  // listeners go before the dispatcher they serve
  instance.stack.reset();
  return converged;
}

}  // namespace lnfs::server
