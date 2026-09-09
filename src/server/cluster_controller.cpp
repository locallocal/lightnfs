#include "server/cluster_controller.hpp"

#include <algorithm>
#include <cerrno>
#include <format>
#include <utility>

#include "util/log.hpp"

namespace lnfs::server {

const char* role_name(Role role) {
    switch (role) {
        case Role::kStandby:
            return "standby";
        case Role::kActivating:
            return "activating";
        case Role::kActive:
            return "active";
        case Role::kDraining:
            return "draining";
    }
    return "unknown";
}

ClusterController::ClusterController(const core::ClusterConfig& cfg, ClusterStore& store, Hooks hooks)
    : cfg_(cfg), store_(store), hooks_(std::move(hooks)), node_(core::cluster_node_name(cfg)) {
    if (!hooks_.post) hooks_.post = [](std::function<void()> fn) { fn(); };
    metrics_ = obs::register_text_provider([this](std::string& out) { append_metrics(out); });
}

ClusterController::~ClusterController() {
    stop();
    // The scrape runs providers under the registry lock, so after this returns no
    // scrape can still be inside append_metrics().
    obs::unregister_text_provider(metrics_);
}

void ClusterController::start() {
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] {
        for (;;) {
            tick();
            std::unique_lock lock(wake_mu_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(cfg_.fence_lease_ms), [&] { return stopping_; });
            if (stopping_) return;
        }
    });
}

void ClusterController::stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard lock(wake_mu_);
        stopping_ = true;
    }
    wake_cv_.notify_all();
    thread_.join();
}

bool ClusterController::auto_takeover_allowed() const {
    return cfg_.takeover == "auto" && cfg_.role != "standby";
}

void ClusterController::tick() {
    tick_once();
    if (hooks_.after_tick) hooks_.after_tick();
}

void ClusterController::tick_once() {
    Role role;
    {
        std::lock_guard lock(mu_);
        role = role_;
    }
    switch (role) {
        case Role::kStandby: {
            auto fence = store_.read_fence();
            {
                std::lock_guard lock(mu_);
                if (fence) {
                    fence_ = *fence;
                    fence_seen_ = std::chrono::steady_clock::now();
                }
            }
            if (!fence) {
                LNFS_WARN("cluster: cannot read the fence: {}", errno_name(fence.error()));
                return;
            }
            if (!auto_takeover_allowed()) return;
            // Free, expired, or ours from a previous incarnation: take over.
            bool free = !*fence;
            if (!free) {
                int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                free = (*fence)->node == node_ || now_ms > (*fence)->expires_at_ms + kFenceSkewTolerance.count();
            }
            if (free) (void)begin_activation(false);
            return;
        }
        case Role::kActivating:
        case Role::kActive:
            renew();
            return;
        case Role::kDraining:
            // the posted drain finishes the transition
            return;
    }
}

Result<void> ClusterController::begin_activation(bool force) {
    std::string prev_node;
    {
        std::lock_guard lock(mu_);
        if (role_ != Role::kStandby) return Err(errno_from(EBUSY));
        role_ = Role::kActivating;
        activation_started_ = std::chrono::steady_clock::now();
        renew_failures_ = 0;
        // The record we are about to replace (read by the last Standby tick, at most one
        // lease old): the dead gateway the takeover hooks should evict.
        if (fence_ && fence_->node != node_) prev_node = fence_->node;
    }
    // 1. fence: {node, epoch+1, now+ttl} unless someone else holds a live one.
    auto current = store_.read_epoch();
    if (!current) {
        std::lock_guard lock(mu_);
        role_ = Role::kStandby;
        LNFS_WARN("cluster: cannot read the epoch: {}", errno_name(current.error()));
        return Err(current.error());
    }
    auto fence = store_.acquire_fence(node_, *current + 1, ttl(), force);
    if (!fence) {
        std::lock_guard lock(mu_);
        role_ = Role::kStandby;
        if (fence.error() != errno_from(EBUSY))
            LNFS_WARN("cluster: cannot acquire the fence: {}", errno_name(fence.error()));
        return Err(fence.error());
    }
    // 2. epoch++: from here on every clientid/stateid the failed gateway issued is STALE.
    auto epoch = store_.bump_epoch();
    if (!epoch) {
        LNFS_ERROR("cluster: fence taken but the epoch cannot advance: {}", errno_name(epoch.error()));
        (void)store_.release_fence(node_);
        std::lock_guard lock(mu_);
        role_ = Role::kStandby;
        ++activation_failures_;
        return Err(epoch.error());
    }
    {
        std::lock_guard lock(mu_);
        fence_ = *fence;
        fence_seen_ = std::chrono::steady_clock::now();
        epoch_ = *epoch;
    }
    LNFS_INFO("cluster: {} taking over (epoch {}, fence ttl {} ms{})", node_, *epoch, ttl().count(),
              force ? ", forced" : "");
    // 3+4. the data plane, on its own thread; the fence keeps being renewed meanwhile.
    uint64_t e = *epoch;
    hooks_.post([this, e, prev_node = std::move(prev_node)]() mutable { run_activation(e, std::move(prev_node)); });
    return {};
}

void ClusterController::run_activation(uint64_t epoch, std::string prev_node) {
    if (hooks_.backend_takeover) {
        TakeoverContext ctx{.identity = {cfg_.id, node_, epoch}, .prev_node = std::move(prev_node)};
        if (auto took = hooks_.backend_takeover(ctx); !took)
            LNFS_WARN("cluster: backend takeover hook failed: {} (reclaims will retry on DELAY)",
                      errno_name(took.error()));
    }
    Result<void> activated = hooks_.activate ? hooks_.activate(epoch) : Result<void>{};
    std::lock_guard lock(mu_);
    // stopped or drained meanwhile
    if (role_ != Role::kActivating) return;
    if (activated) {
        role_ = Role::kActive;
        ++takeovers_;
        auto took = std::chrono::steady_clock::now() - activation_started_;
        last_activation_ = std::chrono::duration_cast<std::chrono::milliseconds>(took);
        activation_hist_.observe_us(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(took).count()));
        LNFS_INFO("cluster: {} active (epoch {}, activation {} ms)", node_, epoch, last_activation_.count());
        return;
    }
    LNFS_ERROR("cluster: activation failed: {}; back to standby", errno_name(activated.error()));
    ++activation_failures_;
    role_ = Role::kStandby;
    (void)store_.release_fence(node_);
}

void ClusterController::renew() {
    auto renewed = store_.renew_fence(node_, ttl());
    if (renewed) {
        std::lock_guard lock(mu_);
        renew_failures_ = 0;
        if (fence_) {
            fence_->expires_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count() +
                                    ttl().count();
            fence_seen_ = std::chrono::steady_clock::now();
        }
        return;
    }
    if (renewed.error() == errno_from(EPERM)) {
        // Someone else holds the fence now: the second line of defence behind the VIP.
        if (auto theirs = store_.read_fence(); theirs && *theirs) {
            std::lock_guard lock(mu_);
            fence_ = **theirs;
            fence_seen_ = std::chrono::steady_clock::now();
        }
        begin_draining("fence taken by another node", true);
        return;
    }
    int failures;
    {
        std::lock_guard lock(mu_);
        failures = ++renew_failures_;
    }
    LNFS_WARN("cluster: fence renew failed ({} in a row): {}", failures, errno_name(renewed.error()));
    if (failures >= 3) begin_draining("fence unreachable", true);
}

void ClusterController::begin_draining(const char* why, bool fence_lost) {
    {
        std::lock_guard lock(mu_);
        if (role_ != Role::kActive && role_ != Role::kActivating) return;
        role_ = Role::kDraining;
        if (fence_lost) ++fence_lost_;
    }
    LNFS_WARN("cluster: {} draining: {}", node_, why);
    hooks_.post([this, release = !fence_lost] { run_draining(release); });
}

void ClusterController::run_draining(bool release) {
    if (hooks_.deactivate) hooks_.deactivate();
    if (hooks_.backend_reset) hooks_.backend_reset();
    // Only our own record is released; a fence someone else took stays theirs.
    if (release) (void)store_.release_fence(node_);
    std::lock_guard lock(mu_);
    role_ = Role::kStandby;
    epoch_ = 0;
    LNFS_INFO("cluster: {} standby", node_);
}

Result<void> ClusterController::request_takeover(bool force) {
    {
        std::lock_guard lock(mu_);
        if (role_ != Role::kStandby) return Err(errno_from(EBUSY));
    }
    return begin_activation(force);
}

Result<void> ClusterController::request_standby() {
    {
        std::lock_guard lock(mu_);
        if (role_ != Role::kActive) return Err(errno_from(EINVAL));
    }
    begin_draining("operator request", false);
    return {};
}

ClusterController::Snapshot ClusterController::snapshot() const {
    std::lock_guard lock(mu_);
    Snapshot out;
    out.role = role_;
    out.node = node_;
    out.epoch = epoch_;
    out.fence = fence_;
    out.fence_seen = fence_seen_;
    out.takeovers = takeovers_;
    out.fence_lost = fence_lost_;
    out.activation_failures = activation_failures_;
    out.last_activation = last_activation_;
    return out;
}

Role ClusterController::role() const {
    std::lock_guard lock(mu_);
    return role_;
}

void ClusterController::append_metrics(std::string& out) const {
    const Snapshot snap = snapshot();
    static constexpr Role kRoles[] = {Role::kStandby, Role::kActivating, Role::kActive, Role::kDraining};
    for (Role r : kRoles)
        out += std::format("lightnfs_cluster_role{{role=\"{}\"}} {}\n", role_name(r), r == snap.role ? 1 : 0);
    out += std::format("lightnfs_cluster_epoch {}\n", snap.epoch);
    // Fence: 1 when the record last seen is ours.  Age = seconds since we renewed it
    // (ours) or read it (someone else's); no sample until a record has been seen.
    const bool owned = snap.fence && snap.fence->node == snap.node;
    out += std::format("lightnfs_cluster_fence_owned {}\n", owned ? 1 : 0);
    if (snap.fence) {
        auto age_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - snap.fence_seen)
                .count();
        out += std::format("lightnfs_cluster_fence_age_seconds {:.3f}\n", static_cast<double>(age_ms) / 1000.0);
    }
    out += std::format(
        "lightnfs_cluster_takeovers_total {}\nlightnfs_cluster_fence_lost_total {}\n"
        "lightnfs_cluster_activation_failures_total {}\n",
        snap.takeovers, snap.fence_lost, snap.activation_failures);
    out += "# TYPE lightnfs_cluster_activation_seconds histogram\n";
    obs::append_histogram(out, "lightnfs_cluster_activation_seconds", "", activation_hist_.snapshot());
}

Result<std::vector<std::string>> ClusterController::peers() const {
    auto digests = store_.list_exports_digests();
    if (!digests) return Err(digests.error());
    std::vector<std::string> out;
    out.reserve(digests->size());
    for (auto& [node, digest] : *digests) out.push_back(node);
    std::sort(out.begin(), out.end());
    return out;
}

// ---- FsClusterController (plan 12 C1) -----------------------------------------------

namespace {

int64_t wall_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

FsClusterController::FsClusterController(const core::ClusterConfig& cfg, const core::ExportTable& exports,
                                         ClusterStore& store, core::FsOwnerView& view, Hooks hooks, uint64_t node_epoch)
    : cfg_(cfg),
      store_(store),
      view_(view),
      hooks_(std::move(hooks)),
      node_(core::cluster_node_name(cfg)),
      node_epoch_(node_epoch) {
    if (!hooks_.post) hooks_.post = [](const std::function<void()>& fn) { fn(); };
    // The boot-time set (plan 12 B1); sync_exports follows later versions (plan 12 B3).
    auto set = exports.snapshot();
    for (const auto& entry : set->entries) {
        Fs& fs = fs_[entry->fsid];
        fs.exp = entry;
        fs.nodes = entry->node_list();
    }
    last_.now_ms = started_ms_ = wall_now_ms();
    // Until the first tick has read the store nothing is known to be ours: every export
    // is Unowned in the view (clients wait), never silently served.
    publish(nullptr);
    metrics_ = obs::register_text_provider([this](std::string& out) { append_metrics(out); });
}

FsClusterController::~FsClusterController() {
    obs::unregister_text_provider(metrics_);
    stop();
}

const char* FsClusterController::fs_role_label(const FsState& fs) {
    if (fs.role == Role::kActivating) return "activating";
    return core::fs_role_name(fs.view.role);
}

void FsClusterController::start() {
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] {
        for (;;) {
            tick();
            std::unique_lock lock(wake_mu_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(cfg_.fence_lease_ms), [&] { return stopping_; });
            if (stopping_) return;
        }
    });
}

void FsClusterController::stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard lock(wake_mu_);
        stopping_ = true;
    }
    wake_cv_.notify_all();
    thread_.join();
}

bool FsClusterController::expired(const NodeFences& rec, int64_t now_ms) {
    return now_ms > rec.expires_at_ms + kFenceSkewTolerance.count();
}

std::optional<FsClusterController::Holder> FsClusterController::holder_of(const StoreView& sv, uint32_t fsid) {
    std::optional<Holder> best;
    for (const auto& rec : sv.fences) {
        for (const auto& hold : rec.holds) {
            if (hold.fsid != fsid) continue;
            bool live = !expired(rec, sv.now_ms);
            if (!best || (live && !best->live) || (live == best->live && rec.expires_at_ms > best->rec->expires_at_ms))
                best = Holder{&rec, hold.epoch, live};
            break;
        }
    }
    return best;
}

bool FsClusterController::alive(const StoreView& sv, std::string_view node) {
    for (const auto& rec : sv.fences)
        if (rec.node == node) return !expired(rec, sv.now_ms);
    return false;
}

std::string FsClusterController::migration_target(const Fs& fs, const StoreView& sv) {
    if (!fs.owner || fs.owner->node.empty() || fs.owner->node == fs.last_holder) return {};
    if (!alive(sv, fs.owner->node)) return {};
    return fs.owner->node;
}

bool FsClusterController::our_turn(const core::ExportEntry& exp, const StoreView& sv, bool stuck) const {
    if (cfg_.takeover != "auto") return false;
    // one load; apply() may swap the list under us
    const auto& nodes = exp.node_list();
    auto self = std::find(nodes.begin(), nodes.end(), node_);
    // not a candidate: ctl --force only
    if (self == nodes.end()) return false;
    // the predecessors had their 2 × ttl and did not take it
    if (stuck) return true;
    // Everyone ahead of us in the export's list gets the first chance: their heartbeat
    // record (empty or not) still live means they are up and will take it themselves.
    // No record at all is a node we have not heard from: dead, unless we ourselves are
    // younger than one ttl — then it may simply not have written its first heartbeat.
    const bool settling = sv.now_ms - started_ms_ < ttl().count();
    for (auto it = nodes.begin(); it != self; ++it) {
        bool heard = false;
        for (const auto& rec : sv.fences) {
            if (rec.node != *it) continue;
            heard = true;
            if (!expired(rec, sv.now_ms)) return false;
        }
        if (!heard && settling) return false;
    }
    return true;
}

bool FsClusterController::renew() {
    auto renewed = store_.renew_fences(node_, ttl());
    if (renewed) {
        std::lock_guard lock(mu_);
        renew_failures_ = 0;
        return true;
    }
    int failures;
    std::vector<uint32_t> held;
    {
        std::lock_guard lock(mu_);
        failures = ++renew_failures_;
        if (failures >= 3)
            for (const auto& [fsid, fs] : fs_)
                if (fs.role == Role::kActive || fs.role == Role::kActivating) held.push_back(fsid);
    }
    LNFS_WARN("cluster: fence renew failed ({} in a row): {}", failures, errno_name(renewed.error()));
    for (uint32_t fsid : held) begin_draining(fsid, "fence unreachable", true, false);
    return false;
}

void FsClusterController::tick() {
    tick_once();
    if (hooks_.after_tick) hooks_.after_tick();
}

void FsClusterController::tick_once() {
    // No takeover while we cannot renew: a fence we cannot keep is not worth taking, and
    // an export just drained for that reason must not bounce straight back.
    if (!renew()) {
        publish(nullptr);
        return;
    }
    StoreView sv;
    sv.now_ms = wall_now_ms();
    auto fences = store_.list_fences();
    if (!fences) {
        LNFS_WARN("cluster: cannot read the fence records: {}", errno_name(fences.error()));
        return;
    }
    sv.fences = std::move(*fences);
    if (auto nodes = store_.list_nodes(); nodes) {
        for (auto& [node, address] : *nodes) sv.addresses[node] = address;
    } else {
        LNFS_WARN("cluster: cannot read the node addresses: {}", errno_name(nodes.error()));
    }
    std::vector<uint32_t> fsids;
    {
        std::lock_guard lock(mu_);
        for (const auto& [fsid, fs] : fs_) fsids.push_back(fsid);
    }
    std::map<uint32_t, std::optional<OwnerRecord>> owners;
    for (uint32_t fsid : fsids) {
        auto owner = store_.read_owner(fsid);
        if (owner) owners[fsid] = *owner;
    }

    struct Lost {
        uint32_t fsid;
        std::string why;
    };
    std::vector<Lost> lost;
    struct Take {
        uint32_t fsid;
        const char* reason;
    };
    std::vector<Take> take;
    struct Unlisted {
        uint32_t fsid;
        std::vector<std::string> nodes;
    };
    // served here, no longer in the export's nodes
    std::vector<Unlisted> unlisted;
    {
        std::lock_guard lock(mu_);
        last_ = sv;
        for (auto& [fsid, fs] : fs_) {
            if (auto it = owners.find(fsid); it != owners.end()) fs.owner = it->second;
            auto holder = holder_of(sv, fsid);
            if (holder) {
                fs.fence = FenceRecord{holder->rec->node, holder->epoch, holder->rec->expires_at_ms};
                fs.last_holder = holder->rec->node;
            } else {
                fs.fence.reset();
            }
            const bool ours = holder && holder->live && holder->rec->node == node_;
            switch (fs.role) {
                case Role::kActive:
                case Role::kActivating:
                    if (ours) {
                        if (fs.role == Role::kActive && fs.evict && !fs.removed) unlisted.push_back({fsid, fs.nodes});
                        break;
                    }
                    // Stripped from our record by a forced takeover elsewhere, or our lease
                    // lapsed: the second line of defence behind the referral.
                    lost.push_back({fsid, holder && holder->live ? "fence taken by node " + holder->rec->node
                                                                 : std::string("fence lapsed")});
                    break;
                case Role::kStandby: {
                    const bool theirs = holder && holder->live && holder->rec->node != node_;
                    if (theirs) {
                        fs.held_off = false;
                        fs.unowned_since_ms = 0;
                        break;
                    }
                    // our own live record still names it (drained on a renew
                    // outage, or a restart): nobody else can take it — retake now
                    if (ours) {
                        fs.unowned_since_ms = 0;
                        if (!fs.held_off && cfg_.takeover == "auto") take.push_back({fsid, "takeover"});
                        break;
                    }
                    // Free: lapsed (since the record's expiry) or never held (since first seen).
                    const int64_t since = holder ? holder->rec->expires_at_ms : sv.now_ms;
                    if (fs.unowned_since_ms == 0 || since < fs.unowned_since_ms) fs.unowned_since_ms = since;
                    // released by the operator: not until someone else had it
                    if (fs.held_off) break;
                    const bool stuck = sv.now_ms - fs.unowned_since_ms > stuck_after().count();
                    // A pending migration (plan 12 D1): the owner record names its target.  The
                    // target takes the export at once, ahead of the node order and whatever the
                    // takeover policy says (the operator asked); everyone else leaves it alone —
                    // until it has sat unowned for 2 × ttl (the target died), when the ordinary
                    // rule takes over again.
                    if (std::string target = migration_target(fs, sv); !target.empty()) {
                        if (target == node_)
                            take.push_back({fsid, "migrate"});
                        else if (stuck && our_turn(*fs.exp, sv, stuck))
                            take.push_back({fsid, "takeover"});
                        break;
                    }
                    if (our_turn(*fs.exp, sv, stuck)) take.push_back({fsid, "takeover"});
                    break;
                }
                case Role::kDraining:
                    // the posted drain finishes the transition
                    break;
            }
        }
    }
    for (const auto& l : lost) begin_draining(l.fsid, l.why.c_str(), true, false);
    for (const auto& t : take) (void)begin_activation(t.fsid, false, t.reason);
    // A nodes change that dropped us (plan 12 B3): hand the export over, or keep
    // serving — and say so every tick — until a listed node is alive.
    for (const auto& u : unlisted) {
        if (migrate_off(u.fsid, u.nodes)) continue;
        std::string listed;
        for (const auto& n : u.nodes) listed += (listed.empty() ? "" : ",") + n;
        LNFS_WARN(
            "cluster: {} still serves fsid {} although its nodes are now [{}]: none of them is "
            "alive",
            node_, u.fsid, listed);
    }
    publish(nullptr);
}

bool FsClusterController::migrate_off(uint32_t fsid, const std::vector<std::string>& nodes) {
    for (const auto& candidate : nodes) {
        if (candidate == node_) continue;
        auto moved = request_migrate(fsid, candidate);
        if (moved) {
            LNFS_INFO("cluster: {} no longer listed for fsid {}: handed to {}", node_, fsid, candidate);
            return true;
        }
        // not Active any more, or IO
        if (moved.error() != errno_from(EHOSTDOWN)) return false;
    }
    return false;
}

void FsClusterController::sync_exports(const std::shared_ptr<const core::ExportSet>& set) {
    std::vector<uint32_t> drain, added, dropped;
    std::vector<std::pair<uint32_t, std::vector<std::string>>> unlisted;
    {
        std::lock_guard lock(mu_);
        for (const auto& entry : set->entries) {
            auto it = fs_.find(entry->fsid);
            if (it == fs_.end()) {
                Fs& fs = fs_[entry->fsid];
                fs.exp = entry;
                fs.nodes = entry->node_list();
                added.push_back(entry->fsid);
                continue;
            }
            Fs& fs = it->second;
            // the same object unless the export was replaced
            fs.exp = entry;
            fs.removed = false;
            const auto& nodes = entry->node_list();
            if (nodes == fs.nodes) continue;
            fs.nodes = nodes;
            const bool listed = std::find(nodes.begin(), nodes.end(), node_) != nodes.end();
            const bool serving = fs.role == Role::kActive || fs.role == Role::kActivating;
            // Activating: the tick after it completes hands over
            fs.evict = serving && !listed;
            if (fs.role == Role::kActive && fs.evict) unlisted.emplace_back(entry->fsid, nodes);
        }
        for (auto it = fs_.begin(); it != fs_.end();) {
            Fs& fs = it->second;
            if (set->by_fsid(it->first) || fs.removed) {
                ++it;
                continue;
            }
            if (fs.role == Role::kStandby) {
                dropped.push_back(it->first);
                it = fs_.erase(it);
                continue;
            }
            // Active / Activating: drain first; Draining: erase when done
            fs.removed = true;
            if (fs.role != Role::kDraining) drain.push_back(it->first);
            ++it;
        }
    }
    for (uint32_t fsid : added) LNFS_INFO("cluster: fsid {} added to the export set", fsid);
    for (uint32_t fsid : dropped) {
        LNFS_INFO("cluster: fsid {} removed from the export set", fsid);
        // a lapsed hold of ours, if any
        (void)store_.release_fs_fence(fsid, node_);
    }
    for (uint32_t fsid : drain) begin_draining(fsid, "removed from catalog", false, true);
    for (const auto& [fsid, nodes] : unlisted)
        if (!migrate_off(fsid, nodes))
            LNFS_WARN(
                "cluster: {} is no longer in the nodes of fsid {} and no listed node is alive: "
                "still serving it",
                node_, fsid);
    publish(nullptr);
}

Result<void> FsClusterController::begin_activation(uint32_t fsid, bool force, const char* reason) {
    std::string prev_node;
    {
        std::lock_guard lock(mu_);
        Fs* fs = find(fsid);
        if (!fs) return Err(errno_from(EINVAL));
        if (fs->role != Role::kStandby) return Err(errno_from(EBUSY));
        fs->role = Role::kActivating;
        fs->unowned_since_ms = 0;
        if (fs->fence && fs->fence->node != node_) prev_node = fs->fence->node;
        // A migration's source released its hold: no record names the export any more,
        // but the last holder is who hands it over (its residue, if any, is ours to clear).
        if (!fs->fence && std::string_view(reason) == "migrate" && fs->last_holder != node_)
            prev_node = fs->last_holder;
    }
    auto back_to_remote = [&](Errno error, bool count) {
        std::lock_guard lock(mu_);
        if (Fs* fs = find(fsid)) {
            fs->role = Role::kStandby;
            if (count) ++fs->activation_failures;
        }
        return Err(error);
    };
    // 1. fence: {fsid → node, fs_epoch+1, now+ttl} unless another node's live record
    //    names the export.
    auto current = store_.read_fs_epoch(fsid);
    if (!current) {
        LNFS_WARN("cluster: cannot read the epoch of fsid {}: {}", fsid, errno_name(current.error()));
        return back_to_remote(current.error(), false);
    }
    auto fence = store_.acquire_fs_fence(fsid, node_, *current + 1, ttl(), force);
    if (!fence) {
        if (fence.error() != errno_from(EBUSY))
            LNFS_WARN("cluster: cannot acquire the fence of fsid {}: {}", fsid, errno_name(fence.error()));
        return back_to_remote(fence.error(), false);
    }
    // 2. fs epoch++: the generation the new owner record and the reclaim window carry.
    auto epoch = store_.bump_fs_epoch(fsid);
    if (!epoch) {
        LNFS_ERROR("cluster: fence of fsid {} taken but its epoch cannot advance: {}", fsid, errno_name(epoch.error()));
        (void)store_.release_fs_fence(fsid, node_);
        return back_to_remote(epoch.error(), true);
    }
    {
        std::lock_guard lock(mu_);
        if (Fs* fs = find(fsid)) {
            fs->fence = *fence;
            fs->fs_epoch = *epoch;
            // our record names it from here on
            fs->last_holder = node_;
        }
    }
    LNFS_INFO("cluster: {} taking over fsid {} (fs epoch {}, fence ttl {} ms{}{}{})", node_, fsid, *epoch,
              ttl().count(), force ? ", forced" : "", prev_node.empty() ? "" : ", from " + prev_node,
              std::string_view(reason) == "migrate" ? ", migrated" : "");
    // Activating: Unowned in the view until the data plane is ready
    publish(nullptr);
    // 3+4. the per-export data-plane work, on its own thread; the batched renew keeps the
    //      fence alive meanwhile (our record already names the export).
    uint64_t e = *epoch;
    hooks_.post([this, fsid, e, prev_node = std::move(prev_node), reason = std::string(reason)]() mutable {
        run_activation(fsid, e, std::move(prev_node), std::move(reason));
    });
    return {};
}

void FsClusterController::run_activation(uint32_t fsid, uint64_t fs_epoch, std::string prev_node, std::string reason) {
    {
        std::lock_guard lock(mu_);
        Fs* fs = find(fsid);
        // Drained, removed from the set (plan 12 B3) or shut down since the post: the
        // data-plane work is not worth starting.
        if (!fs || fs->role != Role::kActivating) return;
    }
    if (hooks_.backend_takeover) {
        TakeoverContext ctx{
            .identity = {cfg_.id, node_, fs_epoch}, .prev_node = std::move(prev_node), .reason = std::move(reason)};
        if (auto took = hooks_.backend_takeover(fsid, ctx); !took)
            LNFS_WARN(
                "cluster: backend takeover hook for fsid {} failed: {} (reclaims will retry "
                "on DELAY)",
                fsid, errno_name(took.error()));
    }
    Result<void> activated = hooks_.activate_fs ? hooks_.activate_fs(fsid, fs_epoch) : Result<void>{};
    bool active = false;
    {
        std::lock_guard lock(mu_);
        Fs* fs = find(fsid);
        // drained or shut down meanwhile
        if (!fs || fs->role != Role::kActivating) return;
        if (activated) {
            fs->role = Role::kActive;
            ++fs->takeovers;
            active = true;
        } else {
            fs->role = Role::kStandby;
            fs->fs_epoch = 0;
            ++fs->activation_failures;
        }
    }
    if (active) {
        OwnerRecord owner{node_, cfg_.node_address, fs_epoch};
        if (auto put = store_.put_owner(fsid, owner); !put)
            LNFS_WARN(
                "cluster: cannot record {} as the owner of fsid {}: {} (peers will refer "
                "by the fence)",
                node_, fsid, errno_name(put.error()));
        std::lock_guard lock(mu_);
        if (Fs* fs = find(fsid)) fs->owner = owner;
        LNFS_INFO("cluster: {} serves fsid {} (fs epoch {})", node_, fsid, fs_epoch);
    } else {
        LNFS_ERROR("cluster: activation of fsid {} failed: {}; back to remote", fsid, errno_name(activated.error()));
        (void)store_.release_fs_fence(fsid, node_);
    }
    publish(nullptr);
}

void FsClusterController::begin_draining(uint32_t fsid, const char* why, bool fence_lost, bool release) {
    {
        std::lock_guard lock(mu_);
        Fs* fs = find(fsid);
        if (!fs || (fs->role != Role::kActive && fs->role != Role::kActivating)) return;
        fs->role = Role::kDraining;
        if (fence_lost) ++fs->fence_lost;
    }
    LNFS_WARN("cluster: {} draining fsid {}: {}", node_, fsid, why);
    // referred on from now; the state goes on the posting thread
    publish(nullptr);
    hooks_.post([this, fsid, release] { run_draining(fsid, release); });
}

void FsClusterController::run_draining(uint32_t fsid, bool release) {
    if (hooks_.deactivate_fs) hooks_.deactivate_fs(fsid);
    bool removed = false;
    {
        std::lock_guard lock(mu_);
        if (Fs* fs = find(fsid)) removed = fs->removed;
    }
    // Only our own hold is released; an export someone else took stays theirs.  An
    // export gone from the set (plan 12 B3) always drops our hold: nothing will renew
    // it on purpose and a stale one would block the fsid if it ever came back.
    if (release || removed) (void)store_.release_fs_fence(fsid, node_);
    {
        std::lock_guard lock(mu_);
        if (Fs* fs = find(fsid)) {
            if (fs->removed) {
                // the entry's last controller reference goes with it
                fs_.erase(fsid);
            } else {
                fs->role = Role::kStandby;
                fs->fs_epoch = 0;
                fs->evict = false;
                // our record no longer names it
                if (release) fs->fence.reset();
            }
        }
    }
    LNFS_INFO("cluster: {} no longer serves fsid {}{}", node_, fsid, removed ? " (removed from the export set)" : "");
    publish(nullptr);
}

void FsClusterController::shutdown() {
    std::vector<uint32_t> held;
    {
        std::lock_guard lock(mu_);
        for (auto& [fsid, fs] : fs_) {
            if (fs.role != Role::kActive && fs.role != Role::kActivating) continue;
            fs.role = Role::kDraining;
            held.push_back(fsid);
        }
    }
    if (held.empty()) return;
    publish(nullptr);
    for (uint32_t fsid : held) {
        if (hooks_.deactivate_fs) hooks_.deactivate_fs(fsid);
        (void)store_.release_fs_fence(fsid, node_);
        std::lock_guard lock(mu_);
        if (Fs* fs = find(fsid)) {
            fs->role = Role::kStandby;
            fs->fs_epoch = 0;
        }
    }
    LNFS_INFO("cluster: {} released {} export(s) on exit", node_, held.size());
    publish(nullptr);
}

Result<void> FsClusterController::request_takeover(uint32_t fsid, bool force) {
    {
        std::lock_guard lock(mu_);
        Fs* fs = find(fsid);
        if (!fs) return Err(errno_from(EINVAL));
        if (fs->role != Role::kStandby) return Err(errno_from(EBUSY));
        fs->held_off = false;
    }
    return begin_activation(fsid, force);
}

Result<void> FsClusterController::request_release(uint32_t fsid) {
    {
        std::lock_guard lock(mu_);
        Fs* fs = find(fsid);
        if (!fs || fs->role != Role::kActive) return Err(errno_from(EINVAL));
        fs->held_off = true;
    }
    begin_draining(fsid, "operator request", false, true);
    return {};
}

Result<void> FsClusterController::request_migrate(uint32_t fsid, std::string_view target) {
    uint64_t fs_epoch = 0;
    {
        std::lock_guard lock(mu_);
        Fs* fs = find(fsid);
        if (!fs) return Err(errno_from(EINVAL));
        if (fs->role != Role::kActive) return Err(errno_from(EPERM));
        fs_epoch = fs->fs_epoch;
    }
    if (target == node_ || target.empty()) return Err(errno_from(EINVAL));
    // 1. the target is a registered gateway with a live heartbeat.
    auto nodes = store_.list_nodes();
    if (!nodes) return Err(nodes.error());
    std::string address;
    bool registered = false;
    for (const auto& [node, addr] : *nodes)
        if (node == target) {
            registered = true;
            address = addr;
        }
    if (!registered) return Err(errno_from(EHOSTDOWN));
    auto fences = store_.list_fences();
    if (!fences) return Err(fences.error());
    StoreView sv;
    sv.now_ms = wall_now_ms();
    sv.fences = std::move(*fences);
    for (auto& [node, addr] : *nodes) sv.addresses[node] = addr;
    if (!alive(sv, target)) return Err(errno_from(EHOSTDOWN));
    // 2. Draining: from here the engine refers clients on (still to us, for a moment).
    //    The fresh store read replaces the last tick's, so the view that follows the
    //    release refers to the target by this heartbeat, not a stale one.
    {
        std::lock_guard lock(mu_);
        Fs* fs = find(fsid);
        if (!fs || fs->role != Role::kActive) return Err(errno_from(EPERM));
        fs->role = Role::kDraining;
        last_ = std::move(sv);
    }
    publish(nullptr);
    // 3. the owner record names the target before the fence goes (design 10 §10.7).
    OwnerRecord owner{std::string(target), address, fs_epoch};
    if (auto put = store_.put_owner(fsid, owner); !put) {
        LNFS_WARN("cluster: cannot hand fsid {} to {}: owner record not written: {}", fsid, target,
                  errno_name(put.error()));
        {
            std::lock_guard lock(mu_);
            if (Fs* fs = find(fsid); fs && fs->role == Role::kDraining) fs->role = Role::kActive;
        }
        publish(nullptr);
        return Err(put.error());
    }
    {
        std::lock_guard lock(mu_);
        if (Fs* fs = find(fsid)) fs->owner = owner;
        ++migrations_;
    }
    LNFS_INFO("cluster: {} migrating fsid {} to {} ({}): owner record written, draining", node_, fsid, target, address);
    // 4+5. state dropped (LEASE_MOVED armed), fence released, view Remote → target.
    hooks_.post([this, fsid] { run_draining(fsid, true); });
    return {};
}

uint64_t FsClusterController::migrations() const {
    std::lock_guard lock(mu_);
    return migrations_;
}

void FsClusterController::publish(const StoreView* sv) {
    std::lock_guard lock(mu_);
    const StoreView& v = sv ? *sv : last_;
    core::FsOwnerView::Map out;
    for (const auto& [fsid, fs] : fs_) {
        core::FsOwner o;
        switch (fs.role) {
            case Role::kActive:
                o = {core::FsRole::kActive, node_, cfg_.node_address, fs.fs_epoch};
                break;
            case Role::kDraining:
                o = {core::FsRole::kDraining, node_, cfg_.node_address, fs.fs_epoch};
                break;
            case Role::kActivating:
                o = {core::FsRole::kUnowned, node_, "", fs.fs_epoch};
                break;
            case Role::kStandby: {
                auto holder = holder_of(v, fsid);
                if (holder && holder->live && holder->rec->node != node_) {
                    o.role = core::FsRole::kRemote;
                    o.node = holder->rec->node;
                    // The owner record carries the address the owner advertises; until the new
                    // owner has written it, the fence holder's registered address stands in.
                    if (fs.owner && fs.owner->node == o.node) {
                        o.address = fs.owner->address;
                        o.fs_epoch = fs.owner->fs_epoch;
                    } else {
                        if (auto it = v.addresses.find(o.node); it != v.addresses.end()) o.address = it->second;
                        o.fs_epoch = holder->epoch;
                    }
                } else if (std::string target = migration_target(fs, v); !target.empty()) {
                    // Handed over, not yet taken: refer to the target (its next tick takes it;
                    // meanwhile it answers DELAY).
                    o.role = core::FsRole::kRemote;
                    o.node = target;
                    o.address = fs.owner->address;
                    o.fs_epoch = fs.owner->fs_epoch;
                } else {
                    o.role = core::FsRole::kUnowned;
                    // who lapsed
                    if (holder) o.node = holder->rec->node;
                }
                break;
            }
        }
        out.emplace(fsid, std::move(o));
    }
    view_.publish(std::move(out));
}

FsClusterController::Fs* FsClusterController::find(uint32_t fsid) {
    auto it = fs_.find(fsid);
    return it == fs_.end() ? nullptr : &it->second;
}

std::vector<FsClusterController::FsState> FsClusterController::snapshot() const {
    auto view = view_.snapshot();
    std::lock_guard lock(mu_);
    std::vector<FsState> out;
    out.reserve(fs_.size());
    for (const auto& [fsid, fs] : fs_) {
        FsState st{fsid, fs.role, fs.fs_epoch, fs.fence, fs.owner, fs.takeovers, fs.fence_lost, fs.activation_failures,
                   {},   {},      {}};
        if (auto it = view->find(fsid); it != view->end()) st.view = it->second;
        if (fs.exp) {
            st.nodes = fs.exp->node_list();
            st.path = fs.exp->path;
        }
        out.push_back(std::move(st));
    }
    return out;
}

Result<std::vector<std::string>> FsClusterController::peers() const {
    auto nodes = store_.list_nodes();
    if (!nodes) return Err(nodes.error());
    std::vector<std::string> out;
    out.reserve(nodes->size());
    for (auto& [node, address] : *nodes) out.push_back(node);
    std::sort(out.begin(), out.end());
    return out;
}

Result<std::vector<std::pair<std::string, std::string>>> FsClusterController::registry() const {
    auto nodes = store_.list_nodes();
    if (!nodes) return Err(nodes.error());
    std::sort(nodes->begin(), nodes->end());
    return std::move(*nodes);
}

Result<std::vector<std::string>> FsClusterController::alive_peers() const {
    auto fences = store_.list_fences();
    if (!fences) return Err(fences.error());
    const int64_t now_ms = wall_now_ms();
    std::vector<std::string> out;
    for (const auto& rec : *fences)
        if (!expired(rec, now_ms)) out.push_back(rec.node);
    std::sort(out.begin(), out.end());
    return out;
}

void FsClusterController::append_metrics(std::string& out) const {
    static constexpr const char* kLabels[] = {"active", "activating", "draining", "remote", "unowned"};
    out += std::format("lightnfs_cluster_node_epoch {}\nlightnfs_cluster_migrations_total {}\n", node_epoch_,
                       migrations());
    for (const FsState& fs : snapshot()) {
        const char* label = fs_role_label(fs);
        for (const char* l : kLabels)
            out += std::format("lightnfs_cluster_fs_role{{fsid=\"{}\",role=\"{}\"}} {}\n", fs.fsid, l,
                               std::string_view(l) == label ? 1 : 0);
        // The owner as this gateway sees it: ourselves (active / draining) or the remote
        // holder; no sample while nobody holds the export.
        if (fs.view.role != core::FsRole::kUnowned && !fs.view.node.empty())
            out += std::format("lightnfs_cluster_fs_owner{{fsid=\"{}\",node=\"{}\"}} 1\n", fs.fsid, fs.view.node);
        out += std::format(
            "lightnfs_cluster_fs_epoch{{fsid=\"{}\"}} {}\n"
            "lightnfs_cluster_fs_takeovers_total{{fsid=\"{}\"}} {}\n"
            "lightnfs_cluster_fs_fence_lost_total{{fsid=\"{}\"}} {}\n"
            "lightnfs_cluster_fs_activation_failures_total{{fsid=\"{}\"}} {}\n",
            fs.fsid, fs.view.fs_epoch, fs.fsid, fs.takeovers, fs.fsid, fs.fence_lost, fs.fsid, fs.activation_failures);
    }
}

Role FsClusterController::role_of(uint32_t fsid) const {
    std::lock_guard lock(mu_);
    auto it = fs_.find(fsid);
    return it == fs_.end() ? Role::kStandby : it->second.role;
}

}  // namespace lnfs::server
