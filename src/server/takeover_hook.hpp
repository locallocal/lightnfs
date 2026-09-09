#pragma once
// External takeover hook (design 09 §9.7, plan 10 D1): `[cluster] takeover_hook` is an
// executable run once per takeover, after every backend's takeover() — the place for
// privileged storage-side eviction that does not belong in the gateway (Lustre's
// `lctl set_param mdc.*.evict_client=<nid>` of the dead gateway, for instance).
//
// The script gets the takeover in its environment:
//   LNFS_CLUSTER_ID   [cluster] id
//   LNFS_NODE         this gateway's node name
//   LNFS_EPOCH        the epoch minted for this takeover
//   LNFS_PREV_NODE    node named by the fence record we replaced ("" when there was none)
//   LNFS_FSID         the export taken over under active-active (plan 12 C2); "" for a
//                     failover takeover, which moves every export at once
//   LNFS_REASON       "takeover" (the holder is gone or was evicted) or "migrate" (the
//                     previous owner handed the export over on purpose, plan 12 D1)
// It is spawned with no arguments, inherits the daemon's stdout/stderr and the rest of
// the environment, and is killed (SIGKILL) when it outlives `timeout`.  Blocking: run
// from the main loop or an offload thread, never on a reactor.

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "backend/api.hpp"
#include "util/result.hpp"

namespace lnfs::server {

// ok on exit status 0; ETIMEDOUT when killed on timeout; EIO on a non-zero exit or a
// signal death; the spawn errno (ENOENT, EACCES, ...) when it cannot start.  Every
// failure is logged with the script path and what happened.
Result<void> run_takeover_hook(const std::string& path, const backend::ClusterIdentity& id, std::string_view prev_node,
                               std::chrono::milliseconds timeout,
                               uint32_t fsid = 0,  // 0 = not scoped to one export
                               std::string_view reason = "takeover");

}  // namespace lnfs::server
