#pragma once
// Prometheus text-exposition providers for lightnfsd (design 08 §8.3, plan doc 10 §3).
//
// obs/metrics owns the counters/histograms the engines bump on the hot path and the
// registry of "text providers" the scrape walks. The providers that read *other*
// subsystems' statistics — the DRC, the v4 state tables, per-export data-path
// counters, per-backend caches, the runtime's offload pool and reactor loops — sit
// here in server/, the one layer allowed to know all of them, instead of in main.cpp.

namespace lnfs::core {
class ExportTable;
}
namespace lnfs::rpc {
class Drc;
}
namespace lnfs::rt {
class Runtime;
}
namespace lnfs::state {
class StateMgr;
}

namespace lnfs::server {

struct MetricsSources {
  rpc::Drc& drc;
  state::StateMgr& state;
  core::ExportTable& exports;
  rt::Runtime& runtime;
};

// Registers one text provider per group. The referenced objects must outlive every
// scrape (run_server tears the frontend down before they go away).
void register_metrics_providers(const MetricsSources& sources);

}  // namespace lnfs::server
