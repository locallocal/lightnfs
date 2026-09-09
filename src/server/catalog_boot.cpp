#include "server/catalog_boot.hpp"

#include <cerrno>
#include <chrono>
#include <format>
#include <utility>

#include "backend/api.hpp"
#include "core/catalog.hpp"
#include "util/log.hpp"

namespace lnfs::server {
namespace {

int64_t wall_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// validate_config names nothing: which export of `local` it refuses is found by
// validating each on its own.  Empty when the failure is a cross-export rule.
std::string blame_export(const core::Config& local) {
  for (const auto& exp : local.exports) {
    core::Config probe = local;
    probe.exports = {exp};
    if (auto ok = core::validate_config(probe); !ok)
      return std::format("export fsid={} ({}): {}", exp.fsid, exp.path, errno_name(ok.error()));
  }
  return {};
}

}  // namespace

Result<CatalogBoot> load_catalog_exports(ClusterStore& store, core::Config& local,
                                         std::string* why) {
  local.exports.clear();
  local.exports_from_catalog = false;
  auto fail = [&](std::string reason, Errno error) {
    LNFS_ERROR("catalog: {}", reason);
    if (why) *why = std::move(reason);
    local.exports.clear();
    local.exports_from_catalog = false;
    return Err(error);
  };
  auto doc = store.read_catalog();
  if (!doc)
    return fail("cannot read the catalog: " + std::string(errno_name(doc.error())), doc.error());
  if (!*doc) {
    LNFS_WARN("catalog: none yet; serving no exports until one is published");
    return CatalogBoot{false, 0, core::canonical_exports_digest(local)};
  }
  const uint64_t version = (*doc)->version;
  auto catalog = core::parse_catalog((*doc)->text);
  if (!catalog)
    return fail(std::format("catalog v{} does not parse: {}", version, errno_name(catalog.error())),
                catalog.error());
  if (catalog->meta.version != version)
    return fail(
        std::format("catalog v{}: its [catalog] version says {}", version, catalog->meta.version),
        errno_from(EINVAL));
  std::string reason;
  if (auto ok =
          core::validate_catalog(*catalog, core::cluster_active_active(local.cluster), &reason);
      !ok)
    return fail(std::format("catalog v{}: {}", version, reason), ok.error());
  // This host's side: a [backend_defaults.<backend>] may only carry per-node keys
  // (cluster-wide ones live in the catalog); a backend that takes per-node keys but
  // has no table here runs on the library's defaults, which is worth a line.
  for (const auto& exp : catalog->exports) {
    if (exp.disabled) continue;
    const auto& backend = exp.cfg.backend;
    auto defaults = local.backend_defaults.find(backend);
    if (defaults == local.backend_defaults.end()) {
      if (backend == "cephfs" || backend == "gluster")
        LNFS_WARN(
            "catalog v{}: export fsid={} ({}): no [backend_defaults.{}] on this host, "
            "the {} library defaults apply",
            version, exp.cfg.fsid, exp.cfg.path, backend, backend);
      continue;
    }
    for (const auto& [key, value] : defaults->second.values)
      if (!core::per_node_backend_key(key))
        return fail(std::format("catalog v{}: export fsid={} ({}): [backend_defaults.{}] key "
                                "\"{}\" is not a per-node key; cluster-wide keys belong in the "
                                "catalog's [export.{}] table",
                                version, exp.cfg.fsid, exp.cfg.path, backend, key, backend),
                    errno_from(EINVAL));
  }
  auto merged = core::merge_with_local(*catalog, local);
  if (!merged)
    return fail(std::format("catalog v{}: cannot merge this host's [backend_defaults]: {}", version,
                            errno_name(merged.error())),
                merged.error());
  local.exports = std::move(*merged);
  local.exports_from_catalog = true;
  if (auto ok = core::validate_config(local); !ok) {
    std::string blamed = blame_export(local);
    return fail(std::format("catalog v{}: this host cannot serve it: {}", version,
                            blamed.empty() ? std::string(errno_name(ok.error())) : blamed),
                ok.error());
  }
  CatalogBoot boot{true, version, core::canonical_exports_digest(local), std::move(*catalog)};
  LNFS_INFO("catalog v{}: {} export(s) for this host ({} in the catalog), digest {}", version,
            local.exports.size(), boot.catalog.exports.size(), boot.digest);
  return boot;
}

void record_catalog_applied(ClusterStore& store, const std::string& node, uint64_t version,
                            const std::string& digest, const std::string& status) {
  CatalogApplied applied{node, version, digest, wall_now_ms(), status};
  if (auto put = store.put_catalog_applied(applied); !put)
    LNFS_WARN("catalog: cannot record catalog.{} = {} {}: {}", node, version, status,
              errno_name(put.error()));
}

bool check_catalog_consistency(ClusterStore& store, const std::string& node, uint64_t version,
                               const std::string& digest) {
  auto applied = store.list_catalog_applied();
  if (!applied) {
    LNFS_ERROR("cannot read the peers' catalog records: {}", errno_name(applied.error()));
    return false;
  }
  for (const auto& peer : *applied) {
    if (peer.node == node) continue;
    if (peer.version == version && peer.status == "ok") {
      LNFS_INFO("catalog: node {} runs v{} too{}", peer.node, peer.version,
                peer.digest == digest ? "" : " (different export digest: other host keys?)");
      continue;
    }
    LNFS_WARN(
        "catalog: node {} is at v{} ({}), we boot v{}: {} (a rolling apply, or that "
        "node failed — see `cluster catalog status`)",
        peer.node, peer.version, peer.status, version,
        peer.version < version   ? "behind"
        : peer.version > version ? "ahead"
                                 : "in error");
  }
  if (auto put = store.put_exports_digest(node, digest); !put) {
    LNFS_ERROR("cannot publish the export digest to the cluster store: {}",
               errno_name(put.error()));
    return false;
  }
  return true;
}

}  // namespace lnfs::server
