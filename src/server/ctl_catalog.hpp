#pragma once
// `lightnfs-ctl cluster catalog …` (design 11 §11.10, plan 12 D1): the read commands
// over the shared catalog (show / status / history / diff), the two writers (import /
// rollback: CAS commits through ClusterStore::write_catalog, retried on EAGAIN) and
// `apply` (the C2 applier).  Everything here is blocking store / file IO, run by the
// ctl server on the offload pool; the catalog needs only a store, so the commands work
// in local mode too (§11.9 step 1: import the local file before switching).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "server/ctl.hpp"

namespace lnfs::server {

// The whole answer for `cluster catalog <sub> …` (`cmd.args[1] == "catalog"`), text or
// `--json`.  `peer_uid` (SO_PEERCRED of the ctl connection) goes into the audit
// trail: updated_by = "<node> uid=<uid>".  Answers "cluster: not enabled" without a
// store.  Blocking; never call on a reactor.
std::string cluster_catalog_answer(const CtlDeps& deps, const CtlCommand& cmd, std::optional<uint32_t> peer_uid);

// The JSON string escaper the ctl answers share.
std::string ctl_json_escape(std::string_view s);

// How many times a CAS commit is retried after EAGAIN (another writer got in between).
inline constexpr int kCatalogCommitRetries = 3;

}  // namespace lnfs::server
