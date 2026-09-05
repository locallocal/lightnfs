#pragma once
// Prometheus text-exposition providers for lightnfsd (design 08 §8.3, plan doc 10 §3).
//
// obs/metrics owns the counters/histograms the engines bump on the hot path and the
// registry of "text providers" the scrape walks. The providers that read *other*
// subsystems' statistics — the DRC, the v4 state tables, per-export data-path
// counters, per-backend caches, the runtime's offload pool and reactor loops — sit
// here in server/, the one layer allowed to know all of them, instead of in main.cpp.

#include <vector>

#include "obs/metrics.hpp"

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

// Registers one text provider per group; the registration unregisters them all when
// destroyed, which must happen before the referenced objects go away (the data plane
// is torn down and rebuilt on a takeover, plan 10 C1).
class MetricsRegistration {
 public:
  MetricsRegistration() = default;
  explicit MetricsRegistration(std::vector<obs::ProviderHandle> handles)
      : handles_(std::move(handles)) {}
  MetricsRegistration(MetricsRegistration&& o) noexcept : handles_(std::move(o.handles_)) {
    o.handles_.clear();
  }
  MetricsRegistration& operator=(MetricsRegistration&& o) noexcept {
    if (this != &o) {
      reset();
      handles_ = std::move(o.handles_);
      o.handles_.clear();
    }
    return *this;
  }
  ~MetricsRegistration() { reset(); }
  void reset() {
    for (auto h : handles_) obs::unregister_text_provider(h);
    handles_.clear();
  }

 private:
  std::vector<obs::ProviderHandle> handles_;
};

MetricsRegistration register_metrics_providers(const MetricsSources& sources);

}  // namespace lnfs::server
