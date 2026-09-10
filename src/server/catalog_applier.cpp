#include "server/catalog_applier.hpp"

#include <cerrno>
#include <format>
#include <future>
#include <set>
#include <utility>

#include "server/cluster_controller.hpp"
#include "util/log.hpp"

namespace lnfs::server {

CatalogApplier::CatalogApplier(Deps deps, uint64_t applied, core::Catalog catalog, std::string digest)
    : deps_(std::move(deps)),
      applied_(applied),
      latest_(applied),
      digest_(std::move(digest)),
      current_(std::move(catalog)) {
    if (!deps_.post) deps_.post = [](const std::function<void()>& fn) { fn(); };
    metrics_ = obs::register_text_provider([this](std::string& out) { append_metrics(out); });
}

CatalogApplier::~CatalogApplier() {
    // The scrape runs providers under the registry lock: after this returns no scrape
    // is inside append_metrics().
    obs::unregister_text_provider(metrics_);
}

void CatalogApplier::poll() {
    auto doc = deps_.store.read_catalog();
    if (!doc) {
        std::lock_guard lock(mu_);
        if (poll_failures_++ % 60 == 0) LNFS_WARN("catalog: cannot read the catalog: {}", errno_name(doc.error()));
        return;
    }
    const uint64_t latest = *doc ? (*doc)->version : 0;
    bool post_apply = false, post_retire = false;
    {
        std::lock_guard lock(mu_);
        poll_failures_ = 0;
        latest_ = latest;
        pending_ = latest > applied_ ? latest : 0;
        // a failed version: retried on the kRetryEveryPolls-th poll
        bool held_back = false;
        if (pending_ && pending_ == failed_version_ && polls_until_retry_ > 0 && --polls_until_retry_ > 0)
            held_back = true;
        if (pending_ && !held_back && !applying_ && deps_.local.cluster.catalog_refresh == "auto") {
            applying_ = true;
            post_apply = true;
        }
        if (!retiring_ && deps_.exports.retired_pending() > 0) {
            retiring_ = true;
            post_retire = true;
        }
    }
    if (post_apply) {
        LNFS_INFO("catalog: v{} published (applied v{}): applying", latest, applied_);
        deps_.post([this] {
            (void)apply_latest();
            std::lock_guard lock(mu_);
            applying_ = false;
        });
    }
    if (post_retire)
        deps_.post([this] {
            (void)retire_exports();
            std::lock_guard lock(mu_);
            retiring_ = false;
        });
}

Result<uint64_t> CatalogApplier::apply_now(std::chrono::milliseconds timeout) {
    auto done = std::make_shared<std::promise<Result<uint64_t>>>();
    auto result = done->get_future();
    deps_.post([this, done] { done->set_value(apply_latest()); });
    if (result.wait_for(timeout) != std::future_status::ready) return Err(errno_from(ETIMEDOUT));
    return result.get();
}

Result<uint64_t> CatalogApplier::apply_latest() {
    std::string why;
    auto applied = apply_locked_pipeline(why);
    if (applied) return applied;
    uint64_t old;
    std::string digest;
    {
        std::lock_guard lock(mu_);
        ++failures_;
        last_error_ = why;
        failed_version_ = pending_;
        polls_until_retry_ = kRetryEveryPolls;
        old = applied_;
        digest = digest_;
    }
    LNFS_ERROR("catalog: apply failed, still serving v{}: {}", old, why);
    CatalogApplied record{deps_.node, old, digest, 0, "error:" + why};
    record.applied_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    if (auto put = deps_.store.put_catalog_applied(record); !put)
        LNFS_WARN("catalog: cannot record the failure in catalog.{}: {}", deps_.node, errno_name(put.error()));
    return applied;
}

Result<uint64_t> CatalogApplier::apply_locked_pipeline(std::string& why) {
    auto fail = [&](std::string reason, Errno error) {
        why = std::move(reason);
        return Err(error);
    };
    // 1. the latest document
    auto doc = deps_.store.read_catalog();
    if (!doc) return fail("cannot read the catalog: " + errno_name(doc.error()), doc.error());
    const uint64_t version = *doc ? (*doc)->version : 0;
    {
        std::lock_guard lock(mu_);
        latest_ = version;
        if (version <= applied_) {
            pending_ = 0;
            // nothing newer: idempotent
            return applied_;
        }
        // what a failure below is about
        pending_ = version;
    }
    auto next = core::parse_catalog((*doc)->text);
    if (!next)
        return fail(std::format("catalog v{} does not parse: {}", version, errno_name(next.error())), next.error());
    if (next->meta.version != version)
        return fail(std::format("catalog v{}: its [catalog] version says {}", version, next->meta.version),
                    errno_from(EINVAL));
    std::string reason;
    const bool active_active = core::cluster_active_active(deps_.local.cluster);
    if (auto ok = core::validate_catalog(*next, active_active, &reason); !ok)
        return fail(std::format("catalog v{}: {}", version, reason), ok.error());
    // 2. this host's view of it, validated as a local config would be
    core::Config merged = deps_.local;
    auto exports = core::merge_with_local(*next, merged);
    if (!exports)
        return fail(std::format("catalog v{}: cannot merge this host's [backend_defaults]: {}", version,
                                errno_name(exports.error())),
                    exports.error());
    merged.exports = std::move(*exports);
    merged.exports_from_catalog = true;
    if (auto ok = core::validate_config(merged); !ok) {
        std::string blamed;
        for (const auto& exp : merged.exports) {
            core::Config probe = merged;
            probe.exports = {exp};
            if (auto one = core::validate_config(probe); !one) {
                blamed = std::format("export fsid={} ({}): {}", exp.fsid, exp.path, errno_name(one.error()));
                break;
            }
        }
        return fail(std::format("catalog v{}: this host cannot serve it: {}", version,
                                blamed.empty() ? errno_name(ok.error()) : blamed),
                    ok.error());
    }
    // 3. the plan: what changed against the applied document, reconciled with what the
    //    table actually holds so a retried apply never re-adds or re-removes
    core::Catalog current;
    {
        std::lock_guard lock(mu_);
        current = current_;
    }
    auto diff = core::diff_catalog(current, *next);
    if (!diff.rejected.empty()) {
        std::string fsids;
        for (uint32_t fsid : diff.rejected) fsids += (fsids.empty() ? "" : ",") + std::to_string(fsid);
        return fail(std::format("catalog v{}: fsid {} changed path / backend / cluster keys: remove "
                                "and re-add, or use a new fsid",
                                version, fsids),
                    errno_from(EINVAL));
    }
    auto set = deps_.exports.snapshot();
    auto merged_by_fsid = [&](uint32_t fsid) -> const core::ExportConfig* {
        for (const auto& exp : merged.exports)
            if (exp.fsid == fsid) return &exp;
        return nullptr;
    };
    core::ExportSetPlan plan;
    std::set<uint32_t> planned;
    auto want_add = [&](uint32_t fsid) {
        const auto* cfg = merged_by_fsid(fsid);
        // disabled / already planned
        if (!cfg || !planned.insert(fsid).second) return;
        if (set->by_fsid(fsid))
            plan.update.push_back(*cfg);
        else
            plan.add.push_back(*cfg);
    };
    auto want_remove = [&](uint32_t fsid) {
        if (!planned.insert(fsid).second) return;
        if (set->by_fsid(fsid)) plan.remove.push_back(fsid);
    };
    for (uint32_t fsid : diff.removed) want_remove(fsid);
    for (uint32_t fsid : diff.disabled) want_remove(fsid);
    for (uint32_t fsid : diff.added) want_add(fsid);
    for (uint32_t fsid : diff.enabled) want_add(fsid);
    for (uint32_t fsid : diff.nodes_changed) want_add(fsid);
    for (uint32_t fsid : diff.dynamic_changed) want_add(fsid);
    // Anything the table serves that the merged config no longer lists (a disabled
    // export that never made it into a diff list, a partial earlier apply): remove.
    for (const auto& entry : set->entries)
        if (!merged_by_fsid(entry->fsid)) want_remove(entry->fsid);
    // And anything the merged config lists that the table lacks: add.
    for (const auto& exp : merged.exports)
        if (!set->by_fsid(exp.fsid)) want_add(exp.fsid);
    set.reset();
    // 4. backends for the additions: made and started before anything is published
    std::vector<std::unique_ptr<backend::Backend>> started;
    auto stop_started = [&] {
        for (auto& backend : started)
            if (backend && deps_.stop_backend) deps_.stop_backend(*backend);
        started.clear();
    };
    for (auto& cfg : plan.add) {
        cfg.backend_config.path = cfg.path;
        cfg.backend_config.fsid = cfg.fsid;
        const auto* factory = backend::find_backend(cfg.backend);
        if (!factory) {
            stop_started();
            return fail(std::format("catalog v{}: export fsid={}: backend \"{}\" is not in this build", version,
                                    cfg.fsid, cfg.backend),
                        errno_from(ENODEV));
        }
        auto made = factory->make(cfg.backend_config);
        if (!made) {
            stop_started();
            return fail(std::format("catalog v{}: export fsid={} ({}): the {} backend rejected its "
                                    "configuration (details above)",
                                    version, cfg.fsid, cfg.path, cfg.backend),
                        errno_from(EINVAL));
        }
        if (deps_.start_backend) {
            if (auto ok = deps_.start_backend(*made); !ok) {
                stop_started();
                return fail(std::format("catalog v{}: export fsid={} ({}): backend failed to start: {}", version,
                                        cfg.fsid, cfg.path, errno_name(ok.error())),
                            ok.error());
            }
        }
        started.push_back(std::move(made));
    }
    // 5. publish, tell the controller, record
    const size_t adds = plan.add.size(), updates = plan.update.size(), removes = plan.remove.size();
    auto published = deps_.exports.apply(std::move(plan), started, deps_.exports.snapshot()->epoch);
    if (!published) {
        stop_started();
        return fail(
            std::format("catalog v{}: the export table refused the change: {}", version, errno_name(published.error())),
            published.error());
    }
    if (deps_.fs_cluster) deps_.fs_cluster->sync_exports(*published);
    std::string digest = core::canonical_exports_digest(merged);
    {
        std::lock_guard lock(mu_);
        applied_ = version;
        ++applies_;
        if (pending_ <= version) pending_ = 0;
        current_ = std::move(*next);
        digest_ = digest;
        last_error_.clear();
        failed_version_ = 0;
        polls_until_retry_ = 0;
    }
    LNFS_INFO("catalog v{} applied: {} added, {} updated, {} removed; {} export(s) served, digest {}", version, adds,
              updates, removes, (*published)->entries.size(), digest);
    CatalogApplied record{deps_.node, version, digest, 0, "ok"};
    record.applied_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    if (auto put = deps_.store.put_catalog_applied(record); !put)
        LNFS_WARN("catalog: cannot record catalog.{} = {} ok: {}", deps_.node, version, errno_name(put.error()));
    return version;
}

size_t CatalogApplier::retire_exports() {
    auto retired = deps_.exports.take_retired();
    for (auto& entry : retired) {
        if (deps_.stop_backend && entry->backend) deps_.stop_backend(*entry->backend);
        LNFS_INFO("catalog: export fsid={} ({}) retired: backend stopped", entry->fsid, entry->path);
    }
    const size_t stopped = retired.size();
    retired.clear();
    if (auto oldest = deps_.exports.oldest_retired(); oldest) {
        auto now = std::chrono::steady_clock::now();
        if (now - *oldest > deps_.retire_overdue) {
            std::lock_guard lock(mu_);
            if (now - last_overdue_warning_ > deps_.retire_overdue) {
                last_overdue_warning_ = now;
                LNFS_WARN(
                    "catalog: {} removed export(s) still referenced after {} s (a request or "
                    "the controller holds them)",
                    deps_.exports.retired_pending(),
                    std::chrono::duration_cast<std::chrono::seconds>(now - *oldest).count());
            }
        }
    }
    return stopped;
}

CatalogApplier::Status CatalogApplier::status() const {
    std::lock_guard lock(mu_);
    return Status{.applied = applied_,
                  .latest = latest_,
                  .pending = pending_,
                  .applies = applies_,
                  .failures = failures_,
                  .last_error = last_error_,
                  .refresh = deps_.local.cluster.catalog_refresh};
}

void CatalogApplier::append_metrics(std::string& out) const {
    const Status s = status();
    out += std::format(
        "lightnfs_cluster_catalog_version {}\nlightnfs_cluster_catalog_latest_version {}\n"
        "lightnfs_cluster_catalog_pending {}\nlightnfs_cluster_catalog_applies_total {}\n"
        "lightnfs_cluster_catalog_apply_failures_total {}\n",
        s.applied, s.latest, s.pending ? 1 : 0, s.applies, s.failures);
}

uint64_t CatalogApplier::applied() const {
    std::lock_guard lock(mu_);
    return applied_;
}
uint64_t CatalogApplier::latest() const {
    std::lock_guard lock(mu_);
    return latest_;
}
uint64_t CatalogApplier::pending() const {
    std::lock_guard lock(mu_);
    return pending_;
}
std::string CatalogApplier::last_error() const {
    std::lock_guard lock(mu_);
    return last_error_;
}
uint64_t CatalogApplier::applies() const {
    std::lock_guard lock(mu_);
    return applies_;
}
uint64_t CatalogApplier::failures() const {
    std::lock_guard lock(mu_);
    return failures_;
}
std::string CatalogApplier::digest() const {
    std::lock_guard lock(mu_);
    return digest_;
}
bool CatalogApplier::applying() const {
    std::lock_guard lock(mu_);
    return applying_;
}

}  // namespace lnfs::server
