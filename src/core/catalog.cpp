#include "core/catalog.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "backend/api.hpp"
#include "core/config_parse.hpp"
#include "util/log.hpp"

namespace lnfs::core {
namespace {

using namespace detail;  // NOLINT(google-build-using-namespace): the TOML helpers

void sort_by_fsid(std::vector<CatalogExport>& exports) {
    std::stable_sort(exports.begin(), exports.end(),
                     [](const CatalogExport& a, const CatalogExport& b) { return a.cfg.fsid < b.cfg.fsid; });
}

// The subtable keys that are export identity: everything but this host's keys.
std::map<std::string, std::string> cluster_backend_keys(const ExportConfig& cfg) {
    std::map<std::string, std::string> out;
    for (const auto& [key, value] : cfg.backend_config.values)
        if (!per_node_backend_key(key)) out.emplace(key, value);
    return out;
}

void strip_per_node_keys(ExportConfig& cfg) {
    std::erase_if(cfg.backend_config.values, [](const auto& kv) { return per_node_backend_key(kv.first); });
}

// `inner` lives under `outer` ("/a" under "/", "/a/b" under "/a"; not "/ab" under "/a").
bool nested_path(const std::string& outer, const std::string& inner) {
    return inner.size() > outer.size() && inner.starts_with(outer) && (outer == "/" || inner[outer.size()] == '/');
}

}  // namespace

const CatalogExport* Catalog::by_fsid(uint32_t fsid) const {
    for (const auto& exp : exports)
        if (exp.cfg.fsid == fsid) return &exp;
    return nullptr;
}

bool CatalogDiff::empty() const {
    return added.empty() && removed.empty() && disabled.empty() && enabled.empty() && nodes_changed.empty() &&
           dynamic_changed.empty() && rejected.empty();
}

Result<Catalog> parse_catalog(std::string_view text) {
    Catalog catalog;
    bool header = false, version = false, in_header = false;
    std::optional<ExportBlockParser> block;
    std::istringstream input{std::string(text)};
    std::string raw_line;
    while (std::getline(input, raw_line)) {
        std::string clean = strip_comment(raw_line);
        std::string_view line = trim(clean);
        if (line.empty()) continue;
        if (block) {
            if (LNFS_TRY(block->line(line))) continue;
            block.reset();
        }
        if (line == "[[export]]") {
            auto& exp = catalog.exports.emplace_back();
            block.emplace(exp.cfg, &exp.disabled);
            in_header = false;
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            if (line != "[catalog]" || header) return Err(errno_from(EINVAL));
            header = in_header = true;
            continue;
        }
        size_t equal = line.find('=');
        if (!in_header || equal == std::string_view::npos) return Err(errno_from(EINVAL));
        std::string key(trim(line.substr(0, equal)));
        std::string_view value = trim(line.substr(equal + 1));
        auto& meta = catalog.meta;
        if (key == "version") {
            meta.version = LNFS_TRY(uint_value(value));
            version = true;
        } else if (key == "updated_at")
            meta.updated_at = LNFS_TRY(string_value(value));
        else if (key == "updated_by")
            meta.updated_by = LNFS_TRY(string_value(value));
        else if (key == "comment")
            meta.comment = LNFS_TRY(string_value(value));
        else
            return Err(errno_from(EINVAL));
    }
    if (!header || !version) return Err(errno_from(EINVAL));
    for (auto& exp : catalog.exports) {
        for (const auto& [key, unused] : exp.cfg.backend_config.values)
            if (per_node_backend_key(key))
                LNFS_WARN(
                    "catalog export fsid={}: per-node key \"{}\" ignored (it belongs in this "
                    "host's [backend_defaults.{}])",
                    exp.cfg.fsid, key, exp.cfg.backend);
        strip_per_node_keys(exp.cfg);
    }
    sort_by_fsid(catalog.exports);
    return catalog;
}

Result<uint64_t> peek_catalog_version(std::string_view text) {
    bool in_header = false;
    std::istringstream input{std::string(text)};
    std::string raw_line;
    while (std::getline(input, raw_line)) {
        std::string clean = strip_comment(raw_line);
        std::string_view line = trim(clean);
        if (line.empty()) continue;
        if (line.front() == '[') {
            in_header = line == "[catalog]";
            continue;
        }
        size_t equal = line.find('=');
        if (!in_header || equal == std::string_view::npos) continue;
        if (trim(line.substr(0, equal)) == "version") return uint_value(line.substr(equal + 1));
    }
    return Err(errno_from(EINVAL));
}

std::string serialize_catalog(const Catalog& catalog) {
    const auto& meta = catalog.meta;
    std::string out = std::format(
        "[catalog]\nversion = {}\nupdated_at = {}\nupdated_by = {}\n"
        "comment = {}\n",
        meta.version, quote(meta.updated_at), quote(meta.updated_by), quote(meta.comment));
    std::vector<CatalogExport> exports = catalog.exports;
    sort_by_fsid(exports);
    for (const auto& exp : exports) {
        const auto& cfg = exp.cfg;
        const char* squash = cfg.squash == Squash::kNone ? "none" : cfg.squash == Squash::kAll ? "all" : "root";
        out += std::format(
            "\n[[export]]\npath = {}\nfsid = {}\nbackend = {}\nnodes = {}\nclients = {}\n"
            "readonly = {}\nsquash = \"{}\"\nanon_uid = {}\nanon_gid = {}\nread_bps = {}\n"
            "write_bps = {}\niops = {}\ndisabled = {}\n",
            quote(cfg.path), cfg.fsid, quote(cfg.backend), quote_array(cfg.nodes), quote_array(cfg.clients),
            cfg.readonly, squash, cfg.anon_uid, cfg.anon_gid, cfg.read_bps, cfg.write_bps, cfg.iops, exp.disabled);
        // sorted; per-node keys never leave a host
        auto keys = cluster_backend_keys(cfg);
        if (keys.empty()) continue;
        out += std::format("[export.{}]\n", cfg.backend);
        for (const auto& [key, value] : keys) out += std::format("{} = {}\n", key, backend_value_text(value));
    }
    return out;
}

Result<std::vector<ExportConfig>> merge_with_local(const Catalog& catalog, const Config& local) {
    std::vector<ExportConfig> out;
    for (const auto& exp : catalog.exports) {
        if (exp.disabled) continue;
        ExportConfig cfg = exp.cfg;
        // per-node keys come from this host only
        strip_per_node_keys(cfg);
        auto defaults = local.backend_defaults.find(cfg.backend);
        if (defaults != local.backend_defaults.end()) {
            for (const auto& [key, value] : defaults->second.values) {
                if (!per_node_backend_key(key)) {
                    LNFS_WARN("[backend_defaults.{}] key \"{}\" is not a per-node key", cfg.backend, key);
                    return Err(errno_from(EINVAL));
                }
                cfg.backend_config.values[key] = value;
            }
        }
        out.push_back(std::move(cfg));
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const ExportConfig& a, const ExportConfig& b) { return a.fsid < b.fsid; });
    return out;
}

Result<void> validate_catalog(const Catalog& catalog, bool active_active, std::string* why) {
    auto fail = [&](std::string reason, int err = EINVAL) {
        if (why)
            *why = std::move(reason);
        else
            LNFS_WARN("catalog: {}", reason);
        return Err(errno_from(err));
    };
    backend::register_builtin_backends();
    std::set<uint32_t> fsids;
    std::vector<const ExportConfig*> exports;
    for (const auto& exp : catalog.exports) {
        const auto& cfg = exp.cfg;
        if (cfg.fsid == 0) return fail(std::format("export path={}: fsid must be non-zero", cfg.path));
        if (!fsids.insert(cfg.fsid).second) return fail(std::format("export fsid={}: fsid is listed twice", cfg.fsid));
        if (cfg.path.empty() || cfg.path.front() != '/' || normalize_path(cfg.path) != cfg.path)
            return fail(std::format("export fsid={}: path must be absolute with no trailing slash", cfg.fsid));
        if (!backend::find_backend(cfg.backend))
            return fail(std::format("export fsid={}: no such backend \"{}\" in this build", cfg.fsid, cfg.backend),
                        ENODEV);
        for (const auto& [key, unused] : cfg.backend_config.values)
            if (per_node_backend_key(key))
                return fail(
                    std::format("export fsid={}: \"{}\" is a per-node key: put it in each "
                                "gateway's [backend_defaults.{}], not in the catalog",
                                cfg.fsid, key, cfg.backend));
        if (active_active && cfg.nodes.empty())
            return fail(std::format("export fsid={}: nodes is required under active-active", cfg.fsid));
        std::string reason;
        if (!valid_export_nodes(cfg, reason)) return fail(std::move(reason));
        if (cfg.clients.empty()) return fail(std::format("export fsid={}: clients must not be empty", cfg.fsid));
        for (const auto& client : cfg.clients)
            if (!Cidr::parse(client))
                return fail(std::format("export fsid={}: bad client CIDR \"{}\"", cfg.fsid, client));
        for (const ExportConfig* other : exports) {
            if (other->path == cfg.path)
                return fail(std::format("export fsid={} and fsid={} share path {}", other->fsid, cfg.fsid, cfg.path));
            const ExportConfig* outer = nested_path(other->path, cfg.path)   ? other
                                        : nested_path(cfg.path, other->path) ? &cfg
                                                                             : nullptr;
            if (outer) {
                const ExportConfig* inner = outer == other ? &cfg : other;
                return fail(
                    std::format("export fsid={} ({}) is nested in fsid={} ({}): export paths "
                                "must not be prefixes of one another",
                                inner->fsid, inner->path, outer->fsid, outer->path));
            }
        }
        exports.push_back(&cfg);
    }
    if (active_active) {
        std::string reason;
        if (!check_same_volume_nodes(exports, reason)) return fail(std::move(reason));
    }
    return {};
}

CatalogDiff diff_catalog(const Catalog& from, const Catalog& to) {
    std::map<uint32_t, const CatalogExport*> before, after;
    for (const auto& exp : from.exports) before[exp.cfg.fsid] = &exp;
    for (const auto& exp : to.exports) after[exp.cfg.fsid] = &exp;
    CatalogDiff diff;
    for (const auto& [fsid, old] : before)
        if (!after.contains(fsid)) diff.removed.push_back(fsid);
    for (const auto& [fsid, now] : after) {
        auto it = before.find(fsid);
        if (it == before.end()) {
            diff.added.push_back(fsid);
            continue;
        }
        const ExportConfig& a = it->second->cfg;
        const ExportConfig& b = now->cfg;
        if (a.path != b.path || a.backend != b.backend || cluster_backend_keys(a) != cluster_backend_keys(b)) {
            diff.rejected.push_back(fsid);
            continue;
        }
        if (!it->second->disabled && now->disabled) diff.disabled.push_back(fsid);
        if (it->second->disabled && !now->disabled) diff.enabled.push_back(fsid);
        if (a.nodes != b.nodes) diff.nodes_changed.push_back(fsid);
        if (a.clients != b.clients || a.read_bps != b.read_bps || a.write_bps != b.write_bps || a.iops != b.iops ||
            a.readonly != b.readonly || a.squash != b.squash || a.anon_uid != b.anon_uid || a.anon_gid != b.anon_gid)
            diff.dynamic_changed.push_back(fsid);
    }
    // map iteration made every list fsid ascending
    return diff;
}

Catalog catalog_from_config(const Config& config) {
    Catalog catalog;
    for (const auto& exp : config.exports) {
        auto& entry = catalog.exports.emplace_back();
        entry.cfg = exp;
        strip_per_node_keys(entry.cfg);
    }
    sort_by_fsid(catalog.exports);
    return catalog;
}

}  // namespace lnfs::core
