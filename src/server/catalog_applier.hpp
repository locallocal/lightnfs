#pragma once
// Following the shared catalog at run time (design 11 §11.4, plan 12 C2): the poll on
// the controllers' tick thread, the apply pipeline on the main loop, the retirement of
// removed exports.  Everything that touches the export table, the backends or the
// controller runs on the main loop through `post`; the tick thread only reads the
// store and posts.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "backend/api.hpp"
#include "core/catalog.hpp"
#include "core/config.hpp"
#include "server/cluster_store.hpp"
#include "util/result.hpp"

namespace lnfs::server {

class FsClusterController;

class CatalogApplier {
 public:
    struct Deps {
        ClusterStore& store;
        core::ExportTable& exports;
        // This host's side of the configuration (CoreState::local_config): [cluster]
        // (catalog_refresh is read from it on every poll) and [backend_defaults].
        const core::Config& local;
        std::string node;
        // Active-active: told about every published set (sync_exports).  Null under failover.
        FsClusterController* fs_cluster = nullptr;
        // Main-loop hand-off (MainLoop::post); empty = inline (tests).
        std::function<void(std::function<void()>)> post;
        // Backend lifecycle on reactor 0 (daemon: run_on_reactor); empty = not started.
        std::function<Result<void>(backend::Backend&)> start_backend;
        std::function<void(backend::Backend&)> stop_backend;
        // How long a retired export may stay referenced before retire_exports warns
        // (10 × lease).
        std::chrono::milliseconds retire_overdue{std::chrono::seconds(900)};
    };

    // `applied` / `catalog` / `digest`: what the boot loaded (CatalogBoot).
    CatalogApplier(Deps deps, uint64_t applied, core::Catalog catalog, std::string digest);

    // Tick thread.  Reads the catalog's version: newer than what is applied → under
    // catalog_refresh = "auto" posts one apply (never two at once), under "manual" only
    // records it as pending.  A version that failed to apply is retried automatically
    // only every kRetryEveryPolls polls (the operator fixes and applies by hand, or the
    // next version supersedes it).  Also posts the retirement sweep while exports are
    // waiting in the table's queue.
    void poll();
    static constexpr int kRetryEveryPolls = 30;

    // Main-loop thread.  Reads the latest catalog and brings the table to it: parse →
    // validate → merge → diff against the applied version → plan (added / enabled →
    // add with a made and started backend; removed / disabled → remove; nodes or dynamic
    // changes → update in place) → ExportTable::apply → sync_exports → catalog.<node> =
    // v ok.  Idempotent: an export the table already has is updated, not re-added.
    // Returns the applied version (unchanged when nothing newer exists).  Any failure
    // keeps the old set — a started backend is stopped again — records catalog.<node> =
    // <old> error:<why>, counts, and is returned (EINVAL for a rejected change:
    // path / backend / cluster keys of an fsid changed, remove and re-add).
    Result<uint64_t> apply_latest();
    // Any thread: posts apply_latest to the main loop and waits for it (ctl).
    Result<uint64_t> apply_now(std::chrono::milliseconds timeout = std::chrono::seconds(60));

    // Main-loop thread.  Stops and frees the retired exports nothing references any
    // more; warns about those still referenced past `retire_overdue`.  Returns how many
    // were stopped.
    size_t retire_exports();

    uint64_t applied() const;
    uint64_t pending() const;  // a newer version seen but not applied (0 = none)
    std::string last_error() const;
    uint64_t failures() const;
    std::string digest() const;  // of what the table serves
    bool applying() const;

 private:
    Result<uint64_t> apply_locked_pipeline(std::string& why);

    Deps deps_;
    mutable std::mutex mu_;
    uint64_t applied_ = 0;
    uint64_t pending_ = 0;
    uint64_t failures_ = 0;
    bool applying_ = false;  // an apply is posted or running
    bool retiring_ = false;  // a retirement sweep is posted
    std::string last_error_;
    uint64_t failed_version_ = 0;  // the version the last failure was for
    int polls_until_retry_ = 0;    // auto retries of failed_version_ wait this many polls
    std::string digest_;
    core::Catalog current_;  // the applied document
    int poll_failures_ = 0;
    std::chrono::steady_clock::time_point last_overdue_warning_{};
};

}  // namespace lnfs::server
