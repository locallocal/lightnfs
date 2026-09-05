#include "server/protocol_stack.hpp"

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <string>

#include "core/boot_epoch.hpp"
#include "server/cluster_store.hpp"
#include "util/log.hpp"

namespace lnfs::server {
namespace {

// Reclaim-list hooks over the shared cluster store (design 09 §9.4): the list the
// active gateway writes is the one the next active gateway arms grace from.  Write
// failures only warn, as with state_dir/clients/ (a lost record costs one client its
// reclaim, never the session).
state::StateMgr::Config::StableStore cluster_stable_store(ClusterStore& store) {
  return {
      .load =
          [&store]() -> std::vector<std::string> {
            auto listed = store.list_clients();
            if (!listed) {
              LNFS_WARN("cannot read the cluster reclaim list: {}", errno_name(listed.error()));
              return {};
            }
            return std::move(*listed);
          },
      .put =
          [&store](std::string_view owner) {
            if (auto ok = store.put_client(owner); !ok)
              LNFS_WARN("cannot persist client record to the cluster store: {}",
                        errno_name(ok.error()));
          },
      .erase =
          [&store](std::string_view owner) {
            if (auto ok = store.erase_client(owner); !ok)
              LNFS_WARN("cannot erase client record from the cluster store: {}",
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
                                    : state::StateMgr::Config::StableStore{}}) {
  nfs3.set_write_verifier(core::verifier_from_epoch(core.epoch));
  nfs3.set_drc(&drc);
  nfs3.register_with(dispatcher);
  mount.register_with(dispatcher);
}

void ProtocolStack::enable_v4(const core::ServerConfig& cfg, CoreState& core,
                              rt::Runtime& runtime) {
  state.load_grace_list();
  // RFC 8881 §2.10.4 identity: default derives from hostname + state_dir so two
  // distinct lightnfs instances never look like trunking paths of one server.
  char host[256] = "lightnfs";
  (void)::gethostname(host, sizeof host - 1);
  std::string derived = std::string(host) + ":" + cfg.state_dir;
  nfs4.emplace(*core.exports, core.key, locks, pseudofs, state,
               cfg.server_owner.empty() ? derived : cfg.server_owner,
               cfg.server_scope.empty() ? derived : cfg.server_scope);
  nfs4->register_with(dispatcher);
  // Off reactor 0 (plan doc 10 §2.6): the auxiliary tasks used to pile onto the same
  // reactor the (old, single) accept loop lived on.
  rt::spawn(state.run_lease_scanner(&lease_stop),
            runtime.reactor(runtime.reactor_count() - 1));
}

}  // namespace lnfs::server
