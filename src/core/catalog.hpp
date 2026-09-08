#pragma once
// Shared export catalog (design 11, plan 12 A2): the cluster-wide export document kept
// in shared_dir/catalog.toml.  Everything here is a pure function over the parsed
// document — reading and writing the shared directory is ClusterStore's job (A3),
// applying a version to the running export table is the daemon's (C2).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/config.hpp"

namespace lnfs::core {

// The `[catalog]` header.  `version` is the CAS token (design 11 §11.3); the rest is
// audit trail filled in by the gateway that commits.
struct CatalogMeta {
  uint64_t version = 0;
  std::string updated_at, updated_by, comment;

  friend bool operator==(const CatalogMeta&, const CatalogMeta&) = default;
};

// One `[[export]]` of the catalog: an ExportConfig minus this host's keys
// (kPerNodeBackendKeys never appear in `cfg.backend_config.values`), plus `disabled`:
// the fsid stays reserved but the export is not served (design 11 §11.5).
struct CatalogExport {
  ExportConfig cfg;
  bool disabled = false;

  friend bool operator==(const CatalogExport&, const CatalogExport&) = default;
};

struct Catalog {
  CatalogMeta meta;
  std::vector<CatalogExport> exports;  // fsid ascending

  const CatalogExport* by_fsid(uint32_t fsid) const;

  friend bool operator==(const Catalog&, const Catalog&) = default;
};

// `[catalog]` header (required, with `version`) + `[[export]]` / `[export.<backend>]`
// blocks in exactly the local file's syntax.  Unknown keys and sections are EINVAL.
// Per-node keys found in a subtable are warned about and dropped (forward compatibility:
// a writer is refused by validate_catalog, a reader tolerates).  Exports come out sorted
// by fsid; duplicates are left for validate_catalog to name.
Result<Catalog> parse_catalog(std::string_view toml);
// Canonical text: fixed key order, every scalar written out, exports fsid ascending,
// subtable keys sorted.  parse_catalog(serialize_catalog(c)) == c for a valid c.
std::string serialize_catalog(const Catalog& catalog);
// Only `[catalog] version`, without parsing the exports: what the store's poll and its
// CAS compare (plan 12 A3).  EINVAL when the header or the key is missing.
Result<uint64_t> peek_catalog_version(std::string_view toml);

// Catalog → this host's export table input: every enabled export with the local
// `[backend_defaults.<its backend>]` keys merged in.  Catalog keys win; per-node keys
// come only from the local side.  Disabled exports are left out (they are not served).
// EINVAL when a backend_defaults table carries a non-per-node key.  The result feeds
// validate_config / ExportTable::build unchanged.
Result<std::vector<ExportConfig>> merge_with_local(const Catalog& catalog, const Config& local);

// Cluster-level rules a writer enforces before committing (design 11 §11.10): fsid
// non-zero and unique, path absolute / unique / no export nested in another, backend
// type known to this build (ENODEV), no per-node keys, `nodes` syntax and (under
// active-active) presence, same-volume exports sharing one owner list (design 10
// §10.6), a parseable non-empty client list.  Disabled exports count for every rule.
// `why` (when given) receives the reason; otherwise it is logged at WARN.
Result<void> validate_catalog(const Catalog& catalog, bool active_active,
                              std::string* why = nullptr);

// Export-level differences between two versions, each list fsid ascending.  `rejected`
// holds fsids whose identity changed (path, backend, or a cluster-wide backend key —
// design 11 §11.5: remove and re-add, or use a new fsid) and appears in no other list;
// the other lists are independent per-field flags, so one fsid may be in several
// (e.g. `enabled` and `nodes_changed`).  `dynamic_changed` covers clients / QoS /
// readonly / squash / anon_*.
struct CatalogDiff {
  std::vector<uint32_t> added, removed, disabled, enabled;
  std::vector<uint32_t> nodes_changed, dynamic_changed, rejected;

  bool empty() const;
  friend bool operator==(const CatalogDiff&, const CatalogDiff&) = default;
};
CatalogDiff diff_catalog(const Catalog& from, const Catalog& to);

// A local Config's exports as a catalog (import and the §11.9 migration): per-node keys
// stripped, nothing disabled, fsid ascending.  `meta` is left at its defaults for the
// caller to fill.  merge_with_local(catalog_from_config(c), c') gives back the same
// canonical_exports_text as c for any c' carrying c's per-node keys as defaults.
Catalog catalog_from_config(const Config& config);

}  // namespace lnfs::core
