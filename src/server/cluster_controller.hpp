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
//
// The controller also owns the cluster metrics (plan 10 C4): one text provider that
// lives as long as the controller (the process, in lightnfsd) rather than the
// protocol stack, so role/epoch/fence/takeover series stay visible while standing by.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "backend/api.hpp"
#include "core/config.hpp"
#include "core/fs_owner_view.hpp"
#include "obs/metrics.hpp"
#include "server/cluster_store.hpp"
#include "util/result.hpp"

namespace lnfs::server {

enum class Role { kStandby, kActivating, kActive, kDraining };
const char* role_name(Role role);

// What a takeover hands to the backends and the external hook (plan 10 D1).
struct TakeoverContext {
  backend::ClusterIdentity identity;  // cluster id, this node, the epoch just minted
  // Node named by the fence record we replaced — the gateway whose storage-side
  // residue the hooks should clear.  Empty when the fence was free (first start) or
  // was our own previous incarnation.
  std::string prev_node;
};

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
    // Storage-side cleanup of the failed gateway's residue (plan 10 D1): every
    // Backend::takeover(), then the external script.  Optional; a failure is logged
    // and the activation continues.
    std::function<Result<void>(const TakeoverContext&)> backend_takeover;
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
  void run_activation(uint64_t epoch, std::string prev_node);  // on the posting thread
  void begin_draining(const char* why, bool fence_lost);
  void run_draining(bool release);       // on the posting thread
  void renew();
  // Prometheus text provider: lightnfs_cluster_* (plan 10 C4).
  void append_metrics(std::string& out) const;
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
  obs::LatencyHistogram activation_hist_;  // Standby → Active, per completed takeover
  obs::ProviderHandle metrics_ = 0;

  std::thread thread_;
  std::mutex wake_mu_;
  std::condition_variable wake_cv_;
  bool stopping_ = false;
};

// ---- active-active: one role per export (design 11 §11.3/§11.14, plan 12 C1) --------
//
// Under `[cluster] mode = "active-active"` every gateway is its own server (plan 12 B1):
// the protocol stack is built once at startup and stays up; what moves between
// gateways is the ownership of single exports.  This controller runs the failover
// machine above once per fsid —
//
//   Remote ──(fence free/expired and our turn, or ctl takeover)──▶ Activating ──▶ Active
//      ▲                                                                          │
//      └────────── Draining ◀──(fence lost, ctl standby/migrate, exit)────────────┘
//
// — over the per-fsid fence records of the store (one `fence.<node>` record lists every
// export a node holds; renewing it is one write per tick however many that is, and an
// empty record is the node's heartbeat).  Every tick renews, reads every record and
// owner, steps each export's machine and publishes a fresh FsOwnerView for the v4
// engine (fs_locations / NFS4ERR_MOVED, plan 12 B2/B3).  The per-export data-plane
// work — arm that export's grace, drop its state on the way out, evict a dead
// gateway's storage residue — goes through three hooks run on the posting thread; the
// stack itself is never rebuilt.  tick() is public so tests drive the machine.
class FsClusterController {
 public:
  struct Hooks {
    // As ClusterController::Hooks::post.  Empty = inline.
    std::function<void(std::function<void()>)> post;
    // This gateway now serves `fsid` with the given fs epoch: arm the export's grace
    // window (StateMgr::load_grace_list(fsid)).  A failure sends the export back to
    // Remote (fence released).
    std::function<Result<void>(uint32_t fsid, uint64_t fs_epoch)> activate_fs;
    // This gateway stops serving `fsid`: drop its open/lock/delegation state
    // (StateMgr::release_fsid).  The view already says Draining when this runs.
    std::function<void(uint32_t fsid)> deactivate_fs;
    // Storage-side eviction scoped to one export (that export's Backend::takeover()
    // and the external hook).  Optional; a failure is logged, the activation goes on.
    std::function<Result<void>(uint32_t fsid, const TakeoverContext&)> backend_takeover;
  };

  struct FsState {
    uint32_t fsid = 0;
    Role role = Role::kStandby;        // kStandby = Remote (served elsewhere or by nobody)
    uint64_t fs_epoch = 0;             // the fs epoch we serve it with (0 unless ours)
    std::optional<FenceRecord> fence;  // the record naming the export last seen
    std::optional<OwnerRecord> owner;  // fs/<fsid>/owner last read
    uint64_t takeovers = 0, fence_lost = 0, activation_failures = 0;
  };

  FsClusterController(const core::ClusterConfig& cfg, const core::ExportTable& exports,
                      ClusterStore& store, core::FsOwnerView& view, Hooks hooks);
  ~FsClusterController();

  // Timer thread: one tick() per fence_lease.  stop() joins it and leaves every role
  // as is; shutdown() then drains what is still Active (see below).
  void start();
  void stop();

  // One step: renew our fence record (the heartbeat), read every record and owner,
  // step each export's machine, publish the view.  Blocking store IO; never on a
  // reactor.
  void tick();

  // Process exit (design 11 §11.7 "planned"): every Active export drains — view says
  // Draining, deactivate_fs, fence released — so clients are referred on rather than
  // left waiting; our record stays behind, empty.  Runs the hooks inline on the
  // calling thread (the main thread, after stop()).
  void shutdown();

  // Operator requests (plan 12 C4 wires them to lightnfs-ctl).  takeover: Remote only;
  // `force` rewrites a live fence held by another node.  release: Active only; drains
  // and releases the fence.  EINVAL for an fsid this gateway does not export.
  Result<void> request_takeover(uint32_t fsid, bool force);
  Result<void> request_release(uint32_t fsid);

  std::vector<FsState> snapshot() const;
  Role role_of(uint32_t fsid) const;  // kStandby for an unknown fsid
  const core::ClusterConfig& config() const { return cfg_; }

 private:
  struct Fs {
    const core::ExportEntry* exp = nullptr;
    Role role = Role::kStandby;
    uint64_t fs_epoch = 0;
    std::optional<FenceRecord> fence;
    std::optional<OwnerRecord> owner;
    uint64_t takeovers = 0, fence_lost = 0, activation_failures = 0;
    // Released by `request_release`: no automatic re-takeover until another node has
    // held the export (or the operator asks again).
    bool held_off = false;
  };
  // What one tick learned from the store: every record, every node's address.
  struct StoreView {
    std::vector<NodeFences> fences;
    std::map<std::string, std::string> addresses;
    int64_t now_ms = 0;
  };
  struct Holder {
    const NodeFences* rec;
    uint64_t epoch;  // the fs epoch that record took the fence with
    bool live;
  };
  static bool expired(const NodeFences& rec, int64_t now_ms);
  // The live record naming `fsid`, else the latest expired one, else nullopt.
  static std::optional<Holder> holder_of(const StoreView& sv, uint32_t fsid);
  // Automatic takeover policy for one export: takeover = auto, we are in its `nodes`,
  // and nobody ahead of us in that list is alive (plan 12 C2 refines this).
  bool our_turn(const core::ExportEntry& exp, const StoreView& sv) const;
  Result<void> begin_activation(uint32_t fsid, bool force);
  void run_activation(uint32_t fsid, uint64_t fs_epoch, std::string prev_node);
  void begin_draining(uint32_t fsid, const char* why, bool fence_lost, bool release);
  void run_draining(uint32_t fsid, bool release);
  bool renew();  // false after a failed renew (three in a row drain what we hold)
  // Rebuilds and publishes the engine's view from the current roles and store view.
  void publish(const StoreView* sv);
  Fs* find(uint32_t fsid);
  std::chrono::milliseconds ttl() const {
    return std::chrono::milliseconds(3 * cfg_.fence_lease_ms);
  }

  core::ClusterConfig cfg_;
  ClusterStore& store_;
  core::FsOwnerView& view_;
  Hooks hooks_;
  std::string node_;

  mutable std::mutex mu_;
  std::map<uint32_t, Fs> fs_;
  StoreView last_;  // the last successful read, for publishes between ticks
  int renew_failures_ = 0;

  std::thread thread_;
  std::mutex wake_mu_;
  std::condition_variable wake_cv_;
  bool stopping_ = false;
};

}  // namespace lnfs::server
