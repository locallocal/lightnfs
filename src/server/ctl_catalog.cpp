#include "server/ctl_catalog.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "core/catalog.hpp"
#include "core/config.hpp"
#include "server/catalog_applier.hpp"
#include "server/cluster_controller.hpp"
#include "server/cluster_store.hpp"
#include "util/log.hpp"

namespace lnfs::server {

std::string ctl_json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                else
                    out += c;
        }
    }
    return out;
}

namespace {

using core::Catalog;
using core::CatalogDiff;
using core::CatalogExport;

std::string error_answer(bool json, std::string_view text) {
    return json ? std::format("{{\"error\":\"{}\"}}\n", ctl_json_escape(text)) : std::format("cluster: {}\n", text);
}

const char* usage(bool json) {
    return json ? "{\"error\":\"bad subcommand\"}\n"
                : "cluster catalog: expected show|status|history|diff [<v1>] [<v2>]|import <file> "
                  "[--dry-run] [--comment <text>]|rollback <version>|apply\n";
}

// "2026-09-10T08:00:00Z"
std::string iso_now() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string iso_of_ms(int64_t ms) {
    if (ms <= 0) return "-";
    const std::time_t secs = ms / 1000;
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// This gateway's name for the audit trail: from whichever controller runs.
std::string node_name(const CtlDeps& deps) {
    if (deps.fs_cluster) return deps.fs_cluster->node();
    if (deps.cluster) return core::cluster_node_name(deps.cluster->config());
    return "?";
}

std::string json_string(std::string_view s) {
    return std::format("\"{}\"", ctl_json_escape(s));
}

std::string json_strings(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) out += (i ? "," : "") + json_string(items[i]);
    return out + "]";
}

std::string json_fsids(const std::vector<uint32_t>& fsids) {
    std::string out = "[";
    for (size_t i = 0; i < fsids.size(); ++i) out += std::format("{}{}", i ? "," : "", fsids[i]);
    return out + "]";
}

std::string text_list(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& s : items) out += (out.empty() ? "" : ",") + s;
    return out.empty() ? std::string("-") : out;
}

std::string text_fsids(const std::vector<uint32_t>& fsids) {
    std::string out;
    for (uint32_t f : fsids) out += std::format("{}{}", out.empty() ? "" : ",", f);
    return out.empty() ? std::string("-") : out;
}

std::string dash_if_empty(const std::string& s) {
    return s.empty() ? std::string("-") : s;
}

const char* squash_name(core::Squash s) {
    return s == core::Squash::kNone ? "none" : s == core::Squash::kAll ? "all" : "root";
}

// The cluster-wide backend keys of one export, sorted (per-node keys never enter a
// catalog, but a hand-edited file might carry one: shown as is).
std::map<std::string, std::string> backend_keys(const core::ExportConfig& cfg) {
    return {cfg.backend_config.values.begin(), cfg.backend_config.values.end()};
}

std::string export_text(const CatalogExport& exp) {
    const auto& c = exp.cfg;
    std::string keys;
    for (const auto& [k, v] : backend_keys(c)) keys += std::format("{}{}={}", keys.empty() ? "" : ",", k, v);
    return std::format(
        "fsid={} path={} backend={} nodes={} disabled={} clients={} readonly={} squash={} "
        "anon_uid={} anon_gid={} read_bps={} write_bps={} iops={} keys={}\n",
        c.fsid, c.path, c.backend, text_list(c.nodes), exp.disabled ? "yes" : "no", text_list(c.clients),
        c.readonly ? "yes" : "no", squash_name(c.squash), c.anon_uid, c.anon_gid, c.read_bps, c.write_bps, c.iops,
        dash_if_empty(keys));
}

std::string export_json(const CatalogExport& exp) {
    const auto& c = exp.cfg;
    std::string keys;
    for (const auto& [k, v] : backend_keys(c))
        keys += std::format("{}{}:{}", keys.empty() ? "" : ",", json_string(k), json_string(v));
    return std::format(
        "{{\"fsid\":{},\"path\":{},\"backend\":{},\"nodes\":{},\"disabled\":{},\"clients\":{},"
        "\"readonly\":{},\"squash\":\"{}\",\"anon_uid\":{},\"anon_gid\":{},\"read_bps\":{},"
        "\"write_bps\":{},\"iops\":{},\"backend_keys\":{{{}}}}}",
        c.fsid, json_string(c.path), json_string(c.backend), json_strings(c.nodes), exp.disabled,
        json_strings(c.clients), c.readonly, squash_name(c.squash), c.anon_uid, c.anon_gid, c.read_bps, c.write_bps,
        c.iops, keys);
}

std::string meta_text(const Catalog& cat) {
    const auto& m = cat.meta;
    return std::format("version={} exports={} updated_at={} updated_by={} comment={}", m.version, cat.exports.size(),
                       dash_if_empty(m.updated_at), dash_if_empty(m.updated_by), dash_if_empty(m.comment));
}

std::string meta_json(const Catalog& cat) {
    const auto& m = cat.meta;
    return std::format("\"version\":{},\"exports\":{},\"updated_at\":{},\"updated_by\":{},\"comment\":{}", m.version,
                       cat.exports.size(), json_string(m.updated_at), json_string(m.updated_by),
                       json_string(m.comment));
}

std::string diff_text(const CatalogDiff& d) {
    return std::format(
        "added={}\nremoved={}\ndisabled={}\nenabled={}\nnodes_changed={}\ndynamic_changed={}\n"
        "rejected={}\n",
        text_fsids(d.added), text_fsids(d.removed), text_fsids(d.disabled), text_fsids(d.enabled),
        text_fsids(d.nodes_changed), text_fsids(d.dynamic_changed), text_fsids(d.rejected));
}

std::string diff_summary(const CatalogDiff& d) {
    return std::format(
        "added={} removed={} disabled={} enabled={} nodes_changed={} dynamic_changed={} "
        "rejected={}",
        text_fsids(d.added), text_fsids(d.removed), text_fsids(d.disabled), text_fsids(d.enabled),
        text_fsids(d.nodes_changed), text_fsids(d.dynamic_changed), text_fsids(d.rejected));
}

std::string diff_json(const CatalogDiff& d) {
    return std::format(
        "\"added\":{},\"removed\":{},\"disabled\":{},\"enabled\":{},\"nodes_changed\":{},"
        "\"dynamic_changed\":{},\"rejected\":{}",
        json_fsids(d.added), json_fsids(d.removed), json_fsids(d.disabled), json_fsids(d.enabled),
        json_fsids(d.nodes_changed), json_fsids(d.dynamic_changed), json_fsids(d.rejected));
}

// The current document, parsed: nullopt = none published yet.  `why` names a store
// or parse failure.
Result<std::optional<Catalog>> read_current(ClusterStore& store, std::string& why) {
    auto doc = store.read_catalog();
    if (!doc) {
        why = std::format("cannot read the catalog: {}", errno_name(doc.error()));
        return Err(doc.error());
    }
    if (!*doc) return std::optional<Catalog>{};
    auto cat = core::parse_catalog((*doc)->text);
    if (!cat) {
        why = std::format("catalog v{} does not parse: {}", (*doc)->version, errno_name(cat.error()));
        return Err(cat.error());
    }
    return std::optional<Catalog>{std::move(*cat)};
}

// Version `v` as a document: 0 = the empty catalog (before any publish), the current
// version from catalog.toml, anything else from the history.
Result<Catalog> read_version(ClusterStore& store, uint64_t v, std::string& why) {
    if (v == 0) return Catalog{};
    auto current = store.read_catalog();
    if (!current) {
        why = std::format("cannot read the catalog: {}", errno_name(current.error()));
        return Err(current.error());
    }
    std::optional<CatalogDoc> doc;
    if (*current && (*current)->version == v) {
        doc = std::move(**current);
    } else {
        auto kept = store.read_catalog_history(v);
        if (!kept) {
            if (kept.error() == errno_from(ENOENT)) {
                auto versions = store.list_catalog_history();
                std::string kept_text = "none";
                if (versions && !versions->empty())
                    kept_text = versions->size() == 1 ? std::format("{}", versions->front())
                                                      : std::format("{}..{}", versions->front(), versions->back());
                why = std::format("version {} is neither current ({}) nor in the history (kept: {})", v,
                                  *current ? std::to_string((*current)->version) : "none", kept_text);
            } else {
                why = std::format("cannot read version {} from the history: {}", v, errno_name(kept.error()));
            }
            return Err(kept.error());
        }
        doc = std::move(*kept);
    }
    auto cat = core::parse_catalog(doc->text);
    if (!cat) {
        why = std::format("catalog v{} does not parse: {}", v, errno_name(cat.error()));
        return Err(cat.error());
    }
    return std::move(*cat);
}

std::optional<uint64_t> parse_version(std::string_view text) {
    if (text == "none" || text == "0") return 0;
    uint64_t v = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    if (text.empty()) return std::nullopt;
    return v;
}

// ---- show ---------------------------------------------------------------------------

std::string show(ClusterStore& store, bool json) {
    std::string why;
    auto cat = read_current(store, why);
    if (!cat) return error_answer(json, why);
    if (!*cat) return json ? "{\"catalog\":null}\n" : "catalog: none\n";
    if (json) {
        std::string rows;
        for (const auto& exp : (*cat)->exports) rows += (rows.empty() ? "" : ",") + export_json(exp);
        return std::format("{{{},\"exports_list\":[{}]}}\n", meta_json(**cat), rows);
    }
    std::string out = meta_text(**cat) + "\n";
    for (const auto& exp : (*cat)->exports) out += export_text(exp);
    return out;
}

// ---- status -------------------------------------------------------------------------

// Every gateway's catalog.<node> beside its heartbeat: under active-active the fence
// records say who is alive; under failover only the fence holder is known to be.
std::string status(const CtlDeps& deps, bool json) {
    ClusterStore& store = *deps.store;
    auto records = store.list_catalog_applied();
    if (!records) return error_answer(json, std::format("cannot list catalog.<node>: {}", errno_name(records.error())));
    std::sort(records->begin(), records->end(),
              [](const CatalogApplied& a, const CatalogApplied& b) { return a.node < b.node; });
    auto latest = store.read_catalog();
    const std::optional<uint64_t> latest_v =
        latest ? std::optional<uint64_t>(*latest ? (*latest)->version : 0) : std::nullopt;
    // alive: "yes" / "no" / "?" (unknown for that node)
    std::optional<std::vector<std::string>> alive_list;
    if (deps.fs_cluster)
        if (auto alive = deps.fs_cluster->alive_peers()) alive_list = std::move(*alive);
    std::optional<ClusterController::Snapshot> failover;
    if (deps.cluster) failover = deps.cluster->snapshot();
    const int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    auto alive_of = [&](const std::string& node) -> const char* {
        if (deps.fs_cluster) {
            if (!alive_list) return "?";
            return std::find(alive_list->begin(), alive_list->end(), node) != alive_list->end() ? "yes" : "no";
        }
        if (failover) {
            if (failover->fence && failover->fence->node == node)
                return failover->fence->expires_at_ms > now_ms ? "yes" : "no";
            if (failover->node == node) return "yes";
        }
        return "?";
    };
    if (json) {
        std::string rows;
        for (const auto& r : *records) {
            const char* alive = alive_of(r.node);
            rows += std::format(
                "{}{{\"node\":{},\"applied\":{},\"alive\":{},\"applied_at_ms\":{},\"digest\":{},\"status\":{}}}",
                rows.empty() ? "" : ",", json_string(r.node), r.version,
                alive[0] == 'y'   ? "true"
                : alive[0] == 'n' ? "false"
                                  : "null",
                r.applied_at_ms, json_string(r.digest), json_string(r.status));
        }
        return std::format("{{\"latest\":{},\"nodes\":[{}]}}\n",
                           latest_v ? (*latest_v ? std::to_string(*latest_v) : "null") : "null", rows);
    }
    std::string out;
    for (const auto& r : *records)
        out +=
            std::format("node={} applied={} alive={} applied_at={} digest={} status={}\n", r.node, r.version,
                        alive_of(r.node), iso_of_ms(r.applied_at_ms), dash_if_empty(r.digest), dash_if_empty(r.status));
    out += std::format("latest={}\n", latest_v ? (*latest_v ? std::to_string(*latest_v) : "none") : "?");
    return out;
}

// ---- history ------------------------------------------------------------------------

std::string history(ClusterStore& store, bool json) {
    auto versions = store.list_catalog_history();
    if (!versions) return error_answer(json, std::format("cannot list the history: {}", errno_name(versions.error())));
    auto current = store.read_catalog();
    if (!current) return error_answer(json, std::format("cannot read the catalog: {}", errno_name(current.error())));
    struct Row {
        uint64_t version;
        std::optional<Catalog> cat;
        bool current;
    };
    std::vector<Row> rows;
    for (uint64_t v : *versions) {
        auto doc = store.read_catalog_history(v);
        std::optional<Catalog> cat;
        if (doc)
            if (auto parsed = core::parse_catalog(doc->text)) cat = std::move(*parsed);
        rows.push_back({v, std::move(cat), false});
    }
    if (*current) {
        std::optional<Catalog> cat;
        if (auto parsed = core::parse_catalog((*current)->text)) cat = std::move(*parsed);
        rows.push_back({(*current)->version, std::move(cat), true});
    }
    if (rows.empty()) return json ? "{\"current\":null,\"versions\":[]}\n" : "catalog history: none\n";
    if (json) {
        std::string list;
        for (const auto& r : rows) {
            list += list.empty() ? "" : ",";
            if (r.cat)
                list += std::format("{{{},\"current\":{}}}", meta_json(*r.cat), r.current);
            else
                list += std::format("{{\"version\":{},\"unparseable\":true,\"current\":{}}}", r.version, r.current);
        }
        return std::format("{{\"current\":{},\"versions\":[{}]}}\n",
                           *current ? std::to_string((*current)->version) : "null", list);
    }
    std::string out;
    for (const auto& r : rows) {
        if (r.cat)
            out += std::format("{} current={}\n", meta_text(*r.cat), r.current ? "yes" : "no");
        else
            out += std::format("version={} unparseable current={}\n", r.version, r.current ? "yes" : "no");
    }
    return out;
}

// ---- diff ---------------------------------------------------------------------------

// `diff [<v1>] [<v2>]`: v2 defaults to the current catalog, v1 to what this gateway
// has applied (its running set), which needs the applier.
std::string diff(const CtlDeps& deps, const CtlCommand& cmd, bool json) {
    ClusterStore& store = *deps.store;
    std::vector<uint64_t> given;
    for (size_t i = 3; i < cmd.args.size(); ++i) {
        auto v = parse_version(cmd.args[i]);
        if (!v) return error_answer(json, std::format("bad version \"{}\": a number or none", cmd.args[i]));
        given.push_back(*v);
    }
    if (given.size() > 2) return error_answer(json, "diff takes at most two versions");
    auto current = store.read_catalog();
    if (!current) return error_answer(json, std::format("cannot read the catalog: {}", errno_name(current.error())));
    const uint64_t current_v = *current ? (*current)->version : 0;
    uint64_t from, to;
    if (given.size() == 2) {
        from = given[0];
        to = given[1];
    } else {
        if (!deps.catalog)
            return error_answer(json,
                                "no applied version here (exports_source = \"local\"): give the two versions "
                                "to compare");
        from = deps.catalog->applied();
        to = given.empty() ? current_v : given[0];
    }
    std::string why;
    auto a = read_version(store, from, why);
    if (!a) return error_answer(json, why);
    auto b = read_version(store, to, why);
    if (!b) return error_answer(json, why);
    const auto d = core::diff_catalog(*a, *b);
    if (json) return std::format("{{\"from\":{},\"to\":{},{}}}\n", from, to, diff_json(d));
    return std::format("from={} to={}\n{}", from, to, diff_text(d));
}

// ---- import / rollback --------------------------------------------------------------

// The file as a catalog: a catalog document (with its [catalog] header) is taken as is,
// a local configuration file is stripped to its exports (catalog_from_config).
Result<Catalog> catalog_from_file(const std::string& path, std::string& why) {
    std::ifstream input(path);
    if (!input) {
        const int e = errno ? errno : ENOENT;
        why = std::format("cannot read {}: {}", path, errno_name(errno_from(e)));
        return Err(errno_from(e));
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string text = contents.str();
    if (core::peek_catalog_version(text)) {
        auto cat = core::parse_catalog(text);
        if (!cat) {
            why = std::format("{} is not a valid catalog document: {}", path, errno_name(cat.error()));
            return Err(cat.error());
        }
        return std::move(*cat);
    }
    auto config = core::parse_config(text);
    if (!config) {
        why = std::format("{} is neither a catalog document nor a valid configuration file: {}", path,
                          errno_name(config.error()));
        return Err(config.error());
    }
    return core::catalog_from_config(*config);
}

struct Commit {
    uint64_t version = 0, was = 0;
    CatalogDiff diff;
};

// Commits `next` as the version after the current one: the header filled in, the
// cluster-level validation, the identity rule (§11.5: a changed path / backend /
// cluster key is refused), then the CAS write — re-read and retried when another
// writer got in first.  `dry_run` stops before the write and reports what it would do.
Result<Commit> commit(const CtlDeps& deps, Catalog next, std::optional<uint32_t> peer_uid, const std::string& comment,
                      bool dry_run, std::string& why) {
    ClusterStore& store = *deps.store;
    const bool active_active = deps.fs_cluster != nullptr;
    if (auto ok = core::validate_catalog(next, active_active, &why); !ok) {
        why = "invalid catalog: " + why;
        return Err(ok.error());
    }
    std::string by = node_name(deps);
    if (peer_uid) by += std::format(" uid={}", *peer_uid);
    for (int attempt = 0;; ++attempt) {
        auto current = read_current(store, why);
        if (!current) return Err(current.error());
        const uint64_t was = *current ? (*current)->meta.version : 0;
        Commit out{.version = was + 1, .was = was, .diff = core::diff_catalog(*current ? **current : Catalog{}, next)};
        if (!out.diff.rejected.empty()) {
            why = std::format("fsid {} changed path / backend / cluster keys: remove and re-add, or use a new fsid",
                              text_fsids(out.diff.rejected));
            return Err(errno_from(EINVAL));
        }
        if (dry_run) return out;
        next.meta =
            core::CatalogMeta{.version = out.version, .updated_at = iso_now(), .updated_by = by, .comment = comment};
        auto wrote = store.write_catalog(was, core::serialize_catalog(next));
        if (wrote) {
            LNFS_INFO("catalog v{} committed by {} ({}): {}", out.version, by, comment.empty() ? "-" : comment,
                      diff_summary(out.diff));
            return out;
        }
        if (wrote.error() != errno_from(EAGAIN) || attempt + 1 >= kCatalogCommitRetries) {
            why = wrote.error() == errno_from(EAGAIN)
                      ? std::format("another writer keeps changing the catalog ({} attempts): retry",
                                    kCatalogCommitRetries)
                      : std::format("cannot write the catalog: {}", errno_name(wrote.error()));
            return Err(wrote.error());
        }
        LNFS_INFO("catalog: v{} was replaced under us; re-reading and retrying", out.version);
    }
}

std::string import_cmd(const CtlDeps& deps, const CtlCommand& cmd, std::optional<uint32_t> peer_uid, bool json) {
    std::string file, comment;
    bool dry_run = false;
    for (size_t i = 3; i < cmd.args.size(); ++i) {
        const auto& a = cmd.args[i];
        if (a == "--dry-run") {
            dry_run = true;
        } else if (a == "--comment") {
            if (i + 1 >= cmd.args.size()) return error_answer(json, "--comment needs a text");
            comment = cmd.args[++i];
        } else if (a.starts_with("--comment=")) {
            comment = a.substr(std::string_view("--comment=").size());
        } else if (file.empty()) {
            file = a;
        } else {
            return error_answer(json, std::format("unexpected argument \"{}\"", a));
        }
    }
    if (file.empty()) return error_answer(json, "import: a gateway-side file is required");
    std::string why;
    auto next = catalog_from_file(file, why);
    if (!next) return error_answer(json, why);
    auto done = commit(deps, std::move(*next), peer_uid, comment, dry_run, why);
    if (!done) return error_answer(json, "import failed: " + why);
    if (json)
        return std::format("{{\"dry_run\":{},\"version\":{},\"was\":{},{}}}\n", dry_run, done->version, done->was,
                           diff_json(done->diff));
    if (dry_run)
        return std::format("catalog import dry-run: would commit v{} (current {}): {}\n", done->version,
                           done->was ? std::format("v{}", done->was) : "none", diff_summary(done->diff));
    return std::format("catalog v{} imported (was {}): {}\n", done->version,
                       done->was ? std::format("v{}", done->was) : "none", diff_summary(done->diff));
}

std::string rollback(const CtlDeps& deps, const CtlCommand& cmd, std::optional<uint32_t> peer_uid, bool json) {
    ClusterStore& store = *deps.store;
    auto v = cmd.args.size() > 3 ? parse_version(cmd.args[3]) : std::nullopt;
    if (!v || *v == 0) return error_answer(json, "rollback: a history version is required");
    std::string comment;
    for (size_t i = 4; i < cmd.args.size(); ++i) {
        if (cmd.args[i] == "--comment" && i + 1 < cmd.args.size())
            comment = cmd.args[++i];
        else if (cmd.args[i].starts_with("--comment="))
            comment = cmd.args[i].substr(10);
    }
    auto current = store.read_catalog();
    if (!current) return error_answer(json, std::format("cannot read the catalog: {}", errno_name(current.error())));
    if (*current && (*current)->version == *v)
        return error_answer(json, std::format("version {} is the current catalog", *v));
    std::string why;
    auto old = read_version(store, *v, why);
    if (!old) return error_answer(json, why);
    if (comment.empty()) comment = std::format("rollback to v{}", *v);
    auto done = commit(deps, std::move(*old), peer_uid, comment, false, why);
    if (!done) return error_answer(json, "rollback failed: " + why);
    if (json)
        return std::format("{{\"version\":{},\"rollback_to\":{},\"was\":{},{}}}\n", done->version, *v, done->was,
                           diff_json(done->diff));
    return std::format("catalog v{} committed: rollback to v{} (was v{}): {}\n", done->version, *v, done->was,
                       diff_summary(done->diff));
}

// ---- apply --------------------------------------------------------------------------

std::string apply(const CtlDeps& deps, bool json) {
    if (!deps.catalog)
        return json ? "{\"error\":\"catalog apply: not enabled\"}\n"
                    : "catalog apply: not enabled (exports_source = \"local\")\n";
    CatalogApplier& applier = *deps.catalog;
    const uint64_t before = applier.applied();
    auto applied = applier.apply_now();
    if (applied) {
        if (json)
            return std::format("{{\"applied\":{},\"changed\":{},\"pending\":{}}}\n", *applied, *applied != before,
                               applier.pending());
        return *applied == before ? std::format("catalog v{} already applied\n", *applied)
                                  : std::format("catalog v{} applied (was v{})\n", *applied, before);
    }
    std::string why = applier.last_error();
    if (applied.error() == errno_from(ETIMEDOUT)) why = "timed out waiting for the main loop";
    return error_answer(json, std::format("catalog apply failed (still v{}): {}", applier.applied(), why));
}

}  // namespace

std::string cluster_catalog_answer(const CtlDeps& deps, const CtlCommand& cmd, std::optional<uint32_t> peer_uid) {
    const bool json = cmd.json;
    if (!deps.store) return json ? "{\"error\":\"not enabled\"}\n" : "cluster: not enabled\n";
    const auto sub = cmd.arg(2);
    if (sub == "show") return show(*deps.store, json);
    if (sub == "status") return status(deps, json);
    if (sub == "history") return history(*deps.store, json);
    if (sub == "diff") return diff(deps, cmd, json);
    if (sub == "import") return import_cmd(deps, cmd, peer_uid, json);
    if (sub == "rollback") return rollback(deps, cmd, peer_uid, json);
    if (sub == "apply") return apply(deps, json);
    return usage(json);
}

}  // namespace lnfs::server
