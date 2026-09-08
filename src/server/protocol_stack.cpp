#include "server/protocol_stack.hpp"

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <future>
#include <string>

#include "core/boot_epoch.hpp"
#include "server/cluster_store.hpp"
#include "util/log.hpp"

namespace lnfs::server {
namespace {

// Reclaim-list hooks over the shared cluster store (design 09 §9.4): the list the
// active gateway writes is the one the next active gateway arms grace from.  fsid 0 is
// the global (failover) list; an export's own list (design 10 §10.3, plan 12 A3) is
// used under active-active.  Write failures only warn, as with state_dir/clients/ (a
// lost record costs one client its reclaim, never the session).
state::StateMgr::Config::StableStore cluster_stable_store(ClusterStore& store) {
  return {
      .load =
          [&store](uint32_t fsid) -> std::vector<std::string> {
            auto listed = fsid == 0 ? store.list_clients() : store.list_clients(fsid);
            if (!listed) {
              LNFS_WARN("cannot read the cluster reclaim list (fsid {}): {}", fsid,
                        errno_name(listed.error()));
              return {};
            }
            return std::move(*listed);
          },
      .put =
          [&store](uint32_t fsid, std::string_view owner) {
            auto ok = fsid == 0 ? store.put_client(owner) : store.put_client(fsid, owner);
            if (!ok)
              LNFS_WARN("cannot persist client record to the cluster store (fsid {}): {}", fsid,
                        errno_name(ok.error()));
          },
      .erase =
          [&store](uint32_t fsid, std::string_view owner) {
            auto ok = fsid == 0 ? store.erase_client(owner) : store.erase_client(fsid, owner);
            if (!ok)
              LNFS_WARN("cannot erase client record from the cluster store (fsid {}): {}", fsid,
                        errno_name(ok.error()));
          },
  };
}

}  // namespace

ProtocolStack::ProtocolStack(const core::ServerConfig& cfg, CoreState& core)
    : drc({.ttl = std::chrono::milliseconds(cfg.drc_ttl_ms), .max_memory = cfg.drc_mem}),
      nfs3(*core.exports, core.key, locks),
      mount(*core.exports, core.key),
      pseudofs(*core.exports, core.epoch),
      state({.boot_epoch = core.epoch,
             .state_dir = cfg.state_dir,
             .lease_seconds = cfg.lease_seconds,
             .grace_seconds = cfg.grace_seconds,
             .courtesy_multiplier = cfg.courtesy_multiplier,
             .max_io = cfg.max_request_size,
             .shards = cfg.state_shards,
             .delegations = cfg.delegations,
             // Native byte-range locks (plan doc 10 §5.3): exports whose backend has
             // native_locks() get every LOCK/LOCKU/LOCKT mirrored into storage.
             .native_locks = {
                 .manager =
                     [exports = core.exports.get()](uint32_t fsid) -> backend::LockMgr* {
                       const auto* entry = exports->by_fsid(fsid);
                       if (!entry) return nullptr;
                       auto native = entry->backend->native_locks();
                       return native ? &native->get() : nullptr;
                     },
                 .resolve =
                     [exports = core.exports.get()](uint32_t fsid, const backend::ObjId& oid)
                         -> rt::Task<Result<backend::ObjPtr>> {
                       const auto* entry = exports->by_fsid(fsid);
                       if (!entry) co_return Err(errno_from(ESTALE));
                       co_return co_await entry->backend->resolve(oid);
                     }},
             .stable = core.cluster ? cluster_stable_store(*core.cluster)
                                    : state::StateMgr::Config::StableStore{},
             .per_fsid_reclaim = core.active_active}) {
  // Active-active: gateways keep independent epochs, so the node name goes into the
  // verifier too — an export that migrates must not look like the same server.
  nfs3.set_write_verifier(core.active_active ? core::verifier_for_node(core.epoch, core.node)
                                             : core::verifier_from_epoch(core.epoch));
  nfs3.set_drc(&drc);
  nfs3.register_with(dispatcher);
  mount.register_with(dispatcher);
}

ServerIdentity derive_server_identity(const core::ServerConfig& cfg,
                                      const core::ClusterConfig& cluster) {
  if (cluster.enabled) {  // config validation rejects explicit owner/scope here
    std::string derived = "lightnfs-cluster:" + cluster.id;
    // Active-active (design 10 §10.2, plan 12 B1): one scope (one administrative
    // domain, the precondition for referrals), but every gateway is its own server —
    // fs_locations sends a client to a *different* server for each export it owns.
    if (core::cluster_active_active(cluster))
      return {derived + ":" + core::cluster_node_name(cluster), derived};
    return {derived, derived};
  }
  char host[256] = "lightnfs";
  (void)::gethostname(host, sizeof host - 1);
  std::string derived = std::string(host) + ":" + cfg.state_dir;
  return {cfg.server_owner.empty() ? derived : cfg.server_owner,
          cfg.server_scope.empty() ? derived : cfg.server_scope};
}

void ProtocolStack::enable_v4(const core::ServerConfig& cfg, const core::ClusterConfig& cluster,
                              CoreState& core, rt::Runtime& runtime) {
  // Active-active arms grace per export when the controller takes it over (plan 12
  // C1); the global window is the single-gateway / failover restart.
  if (!core.active_active) state.load_grace_list();
  auto identity = derive_server_identity(cfg, cluster);
  nfs4.emplace(*core.exports, core.key, locks, pseudofs, state, std::move(identity.owner),
               std::move(identity.scope), core::cluster_active_active(cluster));
  if (core.active_active) nfs4->set_write_verifier(core::verifier_for_node(core.epoch, core.node));
  nfs4->set_owner_view(core.owners);
  nfs4->register_with(dispatcher);
  // Off reactor 0 (plan doc 10 §2.6): the auxiliary tasks used to pile onto the same
  // reactor the (old, single) accept loop lived on.  The wrapper signals the future
  // as its last act so stop_lease_scanner() can join it.
  std::promise<void> exited;
  lease_exited = exited.get_future();
  rt::spawn(
      [](state::StateMgr* s, std::atomic<bool>* stop, std::promise<void> done) -> rt::Task<void> {
        co_await s->run_lease_scanner(stop);
        done.set_value();
      }(&state, &lease_stop, std::move(exited)),
      runtime.reactor(runtime.reactor_count() - 1));
}

void ProtocolStack::stop_lease_scanner() {
  lease_stop.store(true, std::memory_order_release);
  if (lease_exited.valid()) lease_exited.wait();
}

}  // namespace lnfs::server
