#pragma once
// Booting from the shared export catalog (design 11 §11.4, plan 12 C1): what a
// gateway with `[cluster] exports_source = "catalog"` does between building its
// ClusterStore and its export table.  Pure over the store interface so the memory
// store drives it in tests; nothing here touches the data plane.

#include <cstdint>
#include <string>

#include "core/config.hpp"
#include "server/cluster_store.hpp"

namespace lnfs::server {

struct CatalogBoot {
  bool present = false;  // the store holds a catalog
  uint64_t version = 0;  // its version; 0 while absent (the bootstrap case)
  std::string digest;    // canonical_exports_digest of what this host will serve
};

// Reads the store's catalog and turns it into this host's export list: parse, the
// cluster-level validation, [backend_defaults] merged in, `local.exports` filled (with
// `exports_from_catalog` set) and validated as a local config would be.  No catalog
// yet → `local.exports` stays empty and the gateway serves nothing until one is
// published (warned, not an error).  Every failure leaves `local.exports` empty and
// names, through `why` (and the log), the catalog version and the fsid / key at fault
// where there is one.  Nothing is written to the store.
Result<CatalogBoot> load_catalog_exports(ClusterStore& store, core::Config& local,
                                         std::string* why = nullptr);

// catalog.<node> after a boot or an apply: the version, this host's export digest,
// now, and "ok" | "error:<text>".  A store failure only warns (the record is
// informational: `cluster catalog status`).
void record_catalog_applied(ClusterStore& store, const std::string& node, uint64_t version,
                            const std::string& digest, const std::string& status);

// The cluster consistency check in catalog mode (design 11 §11.4 step 5): the peers'
// catalog.<node> versions are logged — behind or ahead is a warning, never a refusal,
// they may be mid-roll — and this host's exports.<node> digest is published for the
// local-mode peers of a mixed cluster (§11.9).  False only when the store cannot be
// read or written.
bool check_catalog_consistency(ClusterStore& store, const std::string& node, uint64_t version,
                               const std::string& digest);

}  // namespace lnfs::server
