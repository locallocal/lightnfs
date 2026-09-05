#pragma once
// Cluster role state machine and fence lease (design 09 §9.5/§9.6, plan 10 C2):
//
//   Standby ──(fence free/expired, or ctl takeover)──▶ Activating ──▶ Active
//      ▲                                                                 │
//      └──────────────── Draining ◀──(fence lost, ctl standby, exit)─────┘
//
// The controller owns the role and the fence; it decides *when* to switch and runs
// the fence IO (blocking calls on a shared filesystem) on its own timer thread,
// never on a reactor.  The data-plane work itself — activate() / deactivate() from
// server/data_plane, the backend takeover and reset hooks — is handed to `post`
// (the main-thread event loop in lightnfsd, an inline call in tests), and the
// controller keeps renewing the fence while that work runs so a slow takeover never
// lets the lease lapse under it.  Every transition is serialized by one mutex;
// tick() is public so tests drive the machine without the thread.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "server/cluster_store.hpp"
#include "util/result.hpp"

namespace lnfs::server {

enum class Role { kStandby, kActivating, kActive, kDraining };
const char* role_name(Role role);

class ClusterController {
 public:
  struct Hooks {
    // Runs work on the thread that owns the data plane (lightnfsd: the main loop).
    // Must not run it inline on the controller's thread while holding its locks —
    // the controller never calls post() with a lock held.  Empty = inline.
    std::function<void(std::function<void()>)> post;
    // Build the stack (with the given epoch) and start listening.
    std::function<Result<void>(uint64_t epoch)> activate;
    // Drain connections, tear the stack down.
    std::function<void()> deactivate;
    // Storage-side cleanup of the failed gateway's residue (plan 10 D1); optional.
    std::function<Result<void>()> backend_takeover;
    // stop()+start() of every backend after draining (design 09 §9.7); optional.
    std::function<void()> backend_reset;
  };

  struct Snapshot {
    Role role = Role::kStandby;
    std::string node;
    uint64_t epoch = 0;                // the epoch this gateway serves (0 until activated)
    std::optional<FenceRecord> fence;  // last fence record seen
    std::chrono::steady_clock::time_point fence_seen{};  // when `fence` was read/written
    uint64_t takeovers = 0;            // activations completed
    uint64_t fence_lost = 0;           // Active → Draining because the fence was taken
    uint64_t activation_failures = 0;  // Activating → Standby
    std::chrono::milliseconds last_activation{0};
  };

  ClusterController(const core::ClusterConfig& cfg, ClusterStore& store, Hooks hooks);
  ~ClusterController();

  // Timer thread: one tick() per fence_lease.  stop() joins it; the role is left as
  // is (the caller drains an Active gateway on exit).
  void start();
  void stop();

  // One step of the machine: Standby polls the fence (and takes over when the policy
  // allows), Activating/Active renew it.  Blocking store IO; never on a reactor.
  void tick();

  // Operator requests (`lightnfs-ctl cluster takeover/standby`, plan 10 C3).
  // takeover: Standby only; `force` rewrites a live fence held by another node.
  // standby: Active only; drains and releases the fence.
  Result<void> request_takeover(bool force);
  Result<void> request_standby();

  Snapshot snapshot() const;
  Role role() const;
  const core::ClusterConfig& config() const { return cfg_; }
  // The gateways known to the store (every node that published an export digest,
  // sorted); blocking store IO, for `lightnfs-ctl cluster status` (plan 10 C3).
  Result<std::vector<std::string>> peers() const;

 private:
  bool auto_takeover_allowed() const;
  // Standby → Activating: fence + epoch, then post the data-plane work.
  Result<void> begin_activation(bool force);
  void run_activation(uint64_t epoch);  // on the posting thread
  void begin_draining(const char* why, bool fence_lost);
  void run_draining(bool release);       // on the posting thread
  void renew();
  std::chrono::milliseconds ttl() const {
    return std::chrono::milliseconds(3 * cfg_.fence_lease_ms);
  }

  core::ClusterConfig cfg_;
  ClusterStore& store_;
  Hooks hooks_;
  std::string node_;

  mutable std::mutex mu_;
  Role role_ = Role::kStandby;
  uint64_t epoch_ = 0;
  std::optional<FenceRecord> fence_;
  std::chrono::steady_clock::time_point fence_seen_{};
  int renew_failures_ = 0;
  uint64_t takeovers_ = 0, fence_lost_ = 0, activation_failures_ = 0;
  std::chrono::steady_clock::time_point activation_started_{};
  std::chrono::milliseconds last_activation_{0};

  std::thread thread_;
  std::mutex wake_mu_;
  std::condition_variable wake_cv_;
  bool stopping_ = false;
};

}  // namespace lnfs::server
