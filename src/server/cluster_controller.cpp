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

}  // namespace lnfs::server
