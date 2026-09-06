#include "server/cluster_controller.hpp"

#include <algorithm>
#include <cerrno>
#include <format>
#include <utility>

#include "util/log.hpp"

namespace lnfs::server {

const char* role_name(Role role) {
  switch (role) {
    case Role::kStandby: return "standby";
    case Role::kActivating: return "activating";
    case Role::kActive: return "active";
    case Role::kDraining: return "draining";
  }
  return "unknown";
}

ClusterController::ClusterController(const core::ClusterConfig& cfg, ClusterStore& store,
                                     Hooks hooks)
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
      wake_cv_.wait_for(lock, std::chrono::milliseconds(cfg_.fence_lease_ms),
                        [&] { return stopping_; });
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
        free = (*fence)->node == node_ ||
               now_ms > (*fence)->expires_at_ms + kFenceSkewTolerance.count();
      }
      if (free) (void)begin_activation(false);
      return;
    }
    case Role::kActivating:
    case Role::kActive:
      renew();
      return;
    case Role::kDraining:
      return;  // the posted drain finishes the transition
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
    LNFS_ERROR("cluster: fence taken but the epoch cannot advance: {}",
               errno_name(epoch.error()));
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
  LNFS_INFO("cluster: {} taking over (epoch {}, fence ttl {} ms{})", node_, *epoch,
            ttl().count(), force ? ", forced" : "");
  // 3+4. the data plane, on its own thread; the fence keeps being renewed meanwhile.
  uint64_t e = *epoch;
  hooks_.post([this, e, prev_node = std::move(prev_node)]() mutable {
    run_activation(e, std::move(prev_node));
  });
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
  if (role_ != Role::kActivating) return;  // stopped or drained meanwhile
  if (activated) {
    role_ = Role::kActive;
    ++takeovers_;
    auto took = std::chrono::steady_clock::now() - activation_started_;
    last_activation_ = std::chrono::duration_cast<std::chrono::milliseconds>(took);
    activation_hist_.observe_us(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(took).count()));
    LNFS_INFO("cluster: {} active (epoch {}, activation {} ms)", node_, epoch,
              last_activation_.count());
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
  LNFS_WARN("cluster: fence renew failed ({} in a row): {}", failures,
            errno_name(renewed.error()));
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
  static constexpr Role kRoles[] = {Role::kStandby, Role::kActivating, Role::kActive,
                                    Role::kDraining};
  for (Role r : kRoles)
    out += std::format("lightnfs_cluster_role{{role=\"{}\"}} {}\n", role_name(r),
                       r == snap.role ? 1 : 0);
  out += std::format("lightnfs_cluster_epoch {}\n", snap.epoch);
  // Fence: 1 when the record last seen is ours.  Age = seconds since we renewed it
  // (ours) or read it (someone else's); no sample until a record has been seen.
  const bool owned = snap.fence && snap.fence->node == snap.node;
  out += std::format("lightnfs_cluster_fence_owned {}\n", owned ? 1 : 0);
  if (snap.fence) {
    auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - snap.fence_seen)
                      .count();
    out += std::format("lightnfs_cluster_fence_age_seconds {:.3f}\n",
                       static_cast<double>(age_ms) / 1000.0);
  }
  out += std::format(
      "lightnfs_cluster_takeovers_total {}\nlightnfs_cluster_fence_lost_total {}\n"
      "lightnfs_cluster_activation_failures_total {}\n",
      snap.takeovers, snap.fence_lost, snap.activation_failures);
  out += "# TYPE lightnfs_cluster_activation_seconds histogram\n";
  obs::append_histogram(out, "lightnfs_cluster_activation_seconds", "",
                        activation_hist_.snapshot());
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
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

FsClusterController::FsClusterController(const core::ClusterConfig& cfg,
                                         const core::ExportTable& exports, ClusterStore& store,
                                         core::FsOwnerView& view, Hooks hooks)
    : cfg_(cfg),
      store_(store),
      view_(view),
      hooks_(std::move(hooks)),
      node_(core::cluster_node_name(cfg)) {
  if (!hooks_.post) hooks_.post = [](const std::function<void()>& fn) { fn(); };
  for (const auto& entry : exports.entries()) fs_[entry->fsid].exp = entry.get();
  last_.now_ms = wall_now_ms();
  // Until the first tick has read the store nothing is known to be ours: every export
  // is Unowned in the view (clients wait), never silently served.
  publish(nullptr);
}

FsClusterController::~FsClusterController() {
  stop();
}

void FsClusterController::start() {
  if (thread_.joinable()) return;
  stopping_ = false;
  thread_ = std::thread([this] {
    for (;;) {
      tick();
      std::unique_lock lock(wake_mu_);
      wake_cv_.wait_for(lock, std::chrono::milliseconds(cfg_.fence_lease_ms),
                        [&] { return stopping_; });
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

std::optional<FsClusterController::Holder> FsClusterController::holder_of(const StoreView& sv,
                                                                          uint32_t fsid) {
  std::optional<Holder> best;
  for (const auto& rec : sv.fences) {
    for (const auto& hold : rec.holds) {
      if (hold.fsid != fsid) continue;
      bool live = !expired(rec, sv.now_ms);
      if (!best || (live && !best->live) ||
          (live == best->live && rec.expires_at_ms > best->rec->expires_at_ms))
        best = Holder{&rec, hold.epoch, live};
      break;
    }
  }
  return best;
}

bool FsClusterController::our_turn(const core::ExportEntry& exp, const StoreView& sv,
                                   bool stuck) const {
  if (cfg_.takeover != "auto") return false;
  auto self = std::find(exp.nodes.begin(), exp.nodes.end(), node_);
  if (self == exp.nodes.end()) return false;  // not a candidate: ctl --force only
  if (stuck) return true;  // the predecessors had their 2 × ttl and did not take it
  // Everyone ahead of us in the export's list gets the first chance: their heartbeat
  // record (empty or not) still live means they are up and will take it themselves.
  for (auto it = exp.nodes.begin(); it != self; ++it) {
    for (const auto& rec : sv.fences)
      if (rec.node == *it && !expired(rec, sv.now_ms)) return false;
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
  std::vector<uint32_t> take;
  {
    std::lock_guard lock(mu_);
    last_ = sv;
    for (auto& [fsid, fs] : fs_) {
      if (auto it = owners.find(fsid); it != owners.end()) fs.owner = it->second;
      auto holder = holder_of(sv, fsid);
      if (holder)
        fs.fence = FenceRecord{holder->rec->node, holder->epoch, holder->rec->expires_at_ms};
      else
        fs.fence.reset();
      const bool ours = holder && holder->live && holder->rec->node == node_;
      switch (fs.role) {
        case Role::kActive:
        case Role::kActivating:
          if (ours) break;
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
          if (ours) {  // our own live record still names it (drained on a renew
                       // outage, or a restart): nobody else can take it — retake now
            fs.unowned_since_ms = 0;
            if (!fs.held_off && cfg_.takeover == "auto") take.push_back(fsid);
            break;
          }
          // Free: lapsed (since the record's expiry) or never held (since first seen).
          const int64_t since = holder ? holder->rec->expires_at_ms : sv.now_ms;
          if (fs.unowned_since_ms == 0 || since < fs.unowned_since_ms) fs.unowned_since_ms = since;
          if (fs.held_off) break;  // released by the operator: not until someone else had it
          const bool stuck = sv.now_ms - fs.unowned_since_ms > stuck_after().count();
          if (our_turn(*fs.exp, sv, stuck)) take.push_back(fsid);
          break;
        }
        case Role::kDraining:
          break;  // the posted drain finishes the transition
      }
    }
  }
  for (const auto& l : lost) begin_draining(l.fsid, l.why.c_str(), true, false);
  for (uint32_t fsid : take) (void)begin_activation(fsid, false);
  publish(nullptr);
}

Result<void> FsClusterController::begin_activation(uint32_t fsid, bool force) {
  std::string prev_node;
  {
    std::lock_guard lock(mu_);
    Fs* fs = find(fsid);
    if (!fs) return Err(errno_from(EINVAL));
    if (fs->role != Role::kStandby) return Err(errno_from(EBUSY));
    fs->role = Role::kActivating;
    fs->unowned_since_ms = 0;
    if (fs->fence && fs->fence->node != node_) prev_node = fs->fence->node;
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
      LNFS_WARN("cluster: cannot acquire the fence of fsid {}: {}", fsid,
                errno_name(fence.error()));
    return back_to_remote(fence.error(), false);
  }
  // 2. fs epoch++: the generation the new owner record and the reclaim window carry.
  auto epoch = store_.bump_fs_epoch(fsid);
  if (!epoch) {
    LNFS_ERROR("cluster: fence of fsid {} taken but its epoch cannot advance: {}", fsid,
               errno_name(epoch.error()));
    (void)store_.release_fs_fence(fsid, node_);
    return back_to_remote(epoch.error(), true);
  }
  {
    std::lock_guard lock(mu_);
    if (Fs* fs = find(fsid)) {
      fs->fence = *fence;
      fs->fs_epoch = *epoch;
    }
  }
  LNFS_INFO("cluster: {} taking over fsid {} (fs epoch {}, fence ttl {} ms{}{})", node_, fsid,
            *epoch, ttl().count(), force ? ", forced" : "",
            prev_node.empty() ? "" : ", from " + prev_node);
  publish(nullptr);  // Activating: Unowned in the view until the data plane is ready
  // 3+4. the per-export data-plane work, on its own thread; the batched renew keeps the
  //      fence alive meanwhile (our record already names the export).
  uint64_t e = *epoch;
  hooks_.post([this, fsid, e, prev_node = std::move(prev_node)]() mutable {
    run_activation(fsid, e, std::move(prev_node));
  });
  return {};
}

void FsClusterController::run_activation(uint32_t fsid, uint64_t fs_epoch, std::string prev_node) {
  if (hooks_.backend_takeover) {
    TakeoverContext ctx{.identity = {cfg_.id, node_, fs_epoch}, .prev_node = std::move(prev_node)};
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
    if (!fs || fs->role != Role::kActivating) return;  // drained or shut down meanwhile
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
    LNFS_ERROR("cluster: activation of fsid {} failed: {}; back to remote", fsid,
               errno_name(activated.error()));
    (void)store_.release_fs_fence(fsid, node_);
  }
  publish(nullptr);
}

void FsClusterController::begin_draining(uint32_t fsid, const char* why, bool fence_lost,
                                         bool release) {
  {
    std::lock_guard lock(mu_);
    Fs* fs = find(fsid);
    if (!fs || (fs->role != Role::kActive && fs->role != Role::kActivating)) return;
    fs->role = Role::kDraining;
    if (fence_lost) ++fs->fence_lost;
  }
  LNFS_WARN("cluster: {} draining fsid {}: {}", node_, fsid, why);
  publish(nullptr);  // referred on from now; the state goes on the posting thread
  hooks_.post([this, fsid, release] { run_draining(fsid, release); });
}

void FsClusterController::run_draining(uint32_t fsid, bool release) {
  if (hooks_.deactivate_fs) hooks_.deactivate_fs(fsid);
  // Only our own hold is released; an export someone else took stays theirs.
  if (release) (void)store_.release_fs_fence(fsid, node_);
  {
    std::lock_guard lock(mu_);
    if (Fs* fs = find(fsid)) {
      fs->role = Role::kStandby;
      fs->fs_epoch = 0;
    }
  }
  LNFS_INFO("cluster: {} no longer serves fsid {}", node_, fsid);
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
        } else {
          o.role = core::FsRole::kUnowned;
          if (holder) o.node = holder->rec->node;  // who lapsed
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
  std::lock_guard lock(mu_);
  std::vector<FsState> out;
  out.reserve(fs_.size());
  for (const auto& [fsid, fs] : fs_)
    out.push_back({fsid, fs.role, fs.fs_epoch, fs.fence, fs.owner, fs.takeovers, fs.fence_lost,
                   fs.activation_failures});
  return out;
}

Role FsClusterController::role_of(uint32_t fsid) const {
  std::lock_guard lock(mu_);
  auto it = fs_.find(fsid);
  return it == fs_.end() ? Role::kStandby : it->second.role;
}

}  // namespace lnfs::server
