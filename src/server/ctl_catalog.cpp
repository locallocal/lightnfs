#include "server/ctl_catalog.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <fstream>
#include <functional>
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

// The change one command makes to the catalog: from what is stored right now (nullopt
// before the first publish) to the document to commit.  Re-run on every CAS retry, so
// an edit lands on the version another writer just committed instead of over it.
using Mutation = std::function<Result<Catalog>(const std::optional<Catalog>& current, std::string& why)>;

// Commits `mutate(current)` as the version after the current one: the cluster-level
// validation, the identity rule (§11.5: a changed path / backend / cluster key of an
// existing fsid is refused), the header filled in, then the CAS write — re-read,
// re-mutated and retried when another writer got in first.  `dry_run` stops before
// the write and reports what it would do.
Result<Commit> commit(const CtlDeps& deps, const Mutation& mutate, std::optional<uint32_t> peer_uid,
                      const std::string& comment, bool dry_run, std::string& why) {
    ClusterStore& store = *deps.store;
    const bool active_active = deps.fs_cluster != nullptr;
    std::string by = node_name(deps);
    if (peer_uid) by += std::format(" uid={}", *peer_uid);
    for (int attempt = 0;; ++attempt) {
        auto current = read_current(store, why);
        if (!current) return Err(current.error());
        auto next = mutate(*current, why);
        if (!next) return Err(next.error());
        if (auto ok = core::validate_catalog(*next, active_active, &why); !ok) {
            why.insert(0, "invalid catalog: ");
            return Err(ok.error());
        }
        const uint64_t was = *current ? (*current)->meta.version : 0;
        Commit out{.version = was + 1, .was = was, .diff = core::diff_catalog(*current ? **current : Catalog{}, *next)};
        if (!out.diff.rejected.empty()) {
            why = std::format("fsid {} changed path / backend / cluster keys: remove and re-add, or use a new fsid",
                              text_fsids(out.diff.rejected));
            return Err(errno_from(EINVAL));
        }
        if (dry_run) return out;
        next->meta =
            core::CatalogMeta{.version = out.version, .updated_at = iso_now(), .updated_by = by, .comment = comment};
        auto wrote = store.write_catalog(was, core::serialize_catalog(*next));
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

// A whole-document replacement (import / rollback): the current version is ignored.
Mutation replace_with(Catalog next) {
    return [next = std::move(next)](const std::optional<Catalog>&, std::string&) -> Result<Catalog> { return next; };
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
    auto done = commit(deps, replace_with(std::move(*next)), peer_uid, comment, dry_run, why);
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
    auto done = commit(deps, replace_with(std::move(*old)), peer_uid, comment, false, why);
    if (!done) return error_answer(json, "rollback failed: " + why);
    if (json)
        return std::format("{{\"version\":{},\"rollback_to\":{},\"was\":{},{}}}\n", done->version, *v, done->was,
                           diff_json(done->diff));
    return std::format("catalog v{} committed: rollback to v{} (was v{}): {}\n", done->version, *v, done->was,
                       diff_summary(done->diff));
}

// ---- export list / add / set / remove (plan 12 D2) ------------------------------------

// `--name=value` and `--name value` both work; `--readonly` / `--disabled` alone mean
// true, `=true|false` says so.  Unknown flags and stray positionals are errors.
struct ExportFlags {
    std::map<std::string, std::string> values;
    // --opt k=v, repeatable
    std::vector<std::pair<std::string, std::string>> opts;
    std::vector<std::string> positionals;
    bool has(std::string_view name) const { return values.contains(std::string(name)); }
    const std::string& get(std::string_view name) const { return values.at(std::string(name)); }
};

inline constexpr std::string_view kValueFlags[] = {"path",   "fsid",     "backend",  "nodes",    "clients",
                                                   "squash", "anon-uid", "anon-gid", "read-bps", "write-bps",
                                                   "iops",   "comment",  "readonly", "disabled"};
inline constexpr std::string_view kBoolFlags[] = {"readonly", "disabled", "force", "dry-run"};

Result<ExportFlags> parse_export_flags(const CtlCommand& cmd, size_t first, std::string& why) {
    ExportFlags out;
    auto is_bool = [](std::string_view n) {
        return std::find(std::begin(kBoolFlags), std::end(kBoolFlags), n) != std::end(kBoolFlags);
    };
    auto takes_value = [](std::string_view n) {
        return std::find(std::begin(kValueFlags), std::end(kValueFlags), n) != std::end(kValueFlags);
    };
    for (size_t i = first; i < cmd.args.size(); ++i) {
        const std::string& a = cmd.args[i];
        if (!a.starts_with("--")) {
            out.positionals.push_back(a);
            continue;
        }
        std::string name = a.substr(2), value;
        bool has_value = false;
        if (auto eq = name.find('='); eq != std::string::npos) {
            value = name.substr(eq + 1);
            name = name.substr(0, eq);
            has_value = true;
        }
        if (name == "opt") {
            if (!has_value) {
                if (i + 1 >= cmd.args.size()) {
                    why = "--opt needs key=value";
                    return Err(errno_from(EINVAL));
                }
                value = cmd.args[++i];
            }
            auto eq = value.find('=');
            if (eq == std::string::npos || eq == 0) {
                why = std::format("--opt \"{}\": expected key=value", value);
                return Err(errno_from(EINVAL));
            }
            out.opts.emplace_back(value.substr(0, eq), value.substr(eq + 1));
            continue;
        }
        if (is_bool(name)) {
            if (!has_value) value = "true";
            if (value != "true" && value != "false") {
                why = std::format("--{} takes true or false", name);
                return Err(errno_from(EINVAL));
            }
        } else if (takes_value(name)) {
            if (!has_value) {
                if (i + 1 >= cmd.args.size()) {
                    why = std::format("--{} needs a value", name);
                    return Err(errno_from(EINVAL));
                }
                value = cmd.args[++i];
            }
        } else {
            why = std::format("unknown flag --{}", name);
            return Err(errno_from(EINVAL));
        }
        out.values[name] = value;
    }
    return out;
}

std::vector<std::string> split_list(std::string_view text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t comma = text.find(',', start);
        if (comma == std::string_view::npos) comma = text.size();
        std::string item(text.substr(start, comma - start));
        if (!item.empty()) out.push_back(std::move(item));
        start = comma + 1;
    }
    return out;
}

std::optional<uint64_t> parse_uint(std::string_view text, uint64_t max) {
    if (text.empty()) return std::nullopt;
    uint64_t v = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<uint64_t>(c - '0');
        if (v > max) return std::nullopt;
    }
    return v;
}

// The on-line-changeable fields of one export from the flags (add and set share them).
Result<void> apply_dynamic_flags(const ExportFlags& f, CatalogExport& exp, std::string& why) {
    auto& c = exp.cfg;
    if (f.has("nodes")) c.nodes = split_list(f.get("nodes"));
    if (f.has("clients")) c.clients = split_list(f.get("clients"));
    if (f.has("readonly")) c.readonly = f.get("readonly") == "true";
    if (f.has("disabled")) exp.disabled = f.get("disabled") == "true";
    if (f.has("squash")) {
        const auto& s = f.get("squash");
        if (s == "none")
            c.squash = core::Squash::kNone;
        else if (s == "root")
            c.squash = core::Squash::kRoot;
        else if (s == "all")
            c.squash = core::Squash::kAll;
        else {
            why = std::format("--squash \"{}\": expected root, all or none", s);
            return Err(errno_from(EINVAL));
        }
    }
    struct Num {
        const char* flag;
        uint64_t max;
        std::function<void(uint64_t)> set;
    };
    const Num nums[] = {
        {"anon-uid", UINT32_MAX, [&](uint64_t v) { c.anon_uid = static_cast<uint32_t>(v); }},
        {"anon-gid", UINT32_MAX, [&](uint64_t v) { c.anon_gid = static_cast<uint32_t>(v); }},
        {"read-bps", UINT64_MAX, [&](uint64_t v) { c.read_bps = v; }},
        {"write-bps", UINT64_MAX, [&](uint64_t v) { c.write_bps = v; }},
        {"iops", UINT32_MAX, [&](uint64_t v) { c.iops = static_cast<uint32_t>(v); }},
    };
    for (const auto& n : nums) {
        if (!f.has(n.flag)) continue;
        auto v = parse_uint(f.get(n.flag), n.max);
        if (!v) {
            why = std::format("--{} \"{}\": expected a number up to {}", n.flag, f.get(n.flag), n.max);
            return Err(errno_from(EINVAL));
        }
        n.set(*v);
    }
    return {};
}

// Who serves `fsid` as this gateway sees it: the owner node under active-active (the
// view; ourselves while activating), this node under failover while Active, empty
// when nobody does.
std::string served_by(const CtlDeps& deps, uint32_t fsid) {
    if (deps.fs_cluster) {
        for (const auto& fs : deps.fs_cluster->snapshot()) {
            if (fs.fsid != fsid) continue;
            if (fs.view.role != core::FsRole::kUnowned && !fs.view.node.empty()) return fs.view.node;
            if (fs.role == Role::kActivating || fs.role == Role::kActive) return deps.fs_cluster->node();
        }
        return "";
    }
    if (deps.cluster && deps.cluster->role() == Role::kActive) return core::cluster_node_name(deps.cluster->config());
    return "";
}

// The fsid reuse rule (design 11 §11.10): an fsid that a kept version used for a
// different export (path / backend / cluster keys) is refused without --force —
// clients may still hold handles minted under the old identity.
Result<void> check_fsid_reuse(ClusterStore& store, const CatalogExport& exp, std::string& why) {
    auto versions = store.list_catalog_history();
    if (!versions) {
        why = std::format("cannot list the history: {}", errno_name(versions.error()));
        return Err(versions.error());
    }
    for (auto it = versions->rbegin(); it != versions->rend(); ++it) {
        auto doc = store.read_catalog_history(*it);
        if (!doc) continue;
        auto cat = core::parse_catalog(doc->text);
        if (!cat) continue;
        const auto* old = cat->by_fsid(exp.cfg.fsid);
        if (!old) continue;
        Catalog before, after;
        before.exports.push_back(*old);
        after.exports.push_back(exp);
        if (core::diff_catalog(before, after).rejected.empty()) return {};
        why = std::format(
            "fsid {} was {} ({}) in v{}: a reused fsid must keep its path, backend and cluster "
            "keys, or use a new fsid (--force to reuse it anyway)",
            exp.cfg.fsid, old->cfg.path, old->cfg.backend, *it);
        return Err(errno_from(EEXIST));
    }
    return {};
}

std::string commit_answer(const Commit& c, uint32_t fsid, const char* action, bool json) {
    if (json)
        return std::format("{{\"version\":{},\"was\":{},\"fsid\":{},\"action\":\"{}\",{}}}\n", c.version, c.was, fsid,
                           action, diff_json(c.diff));
    return std::format("catalog v{} committed (was {}): export fsid={} {}: {}\n", c.version,
                       c.was ? std::format("v{}", c.was) : "none", fsid, action, diff_summary(c.diff));
}

std::string export_list(ClusterStore& store, bool json) {
    std::string why;
    auto cat = read_current(store, why);
    if (!cat) return error_answer(json, why);
    if (!*cat) return json ? "{\"catalog\":null}\n" : "catalog: none\n";
    if (json) {
        std::string rows;
        for (const auto& exp : (*cat)->exports) rows += (rows.empty() ? "" : ",") + export_json(exp);
        return std::format("{{\"version\":{},\"exports_list\":[{}]}}\n", (*cat)->meta.version, rows);
    }
    if ((*cat)->exports.empty()) return std::format("no exports (catalog v{})\n", (*cat)->meta.version);
    std::string out;
    for (const auto& exp : (*cat)->exports) out += export_text(exp);
    return out;
}

std::string export_add(const CtlDeps& deps, const CtlCommand& cmd, std::optional<uint32_t> peer_uid, bool json) {
    std::string why;
    auto flags = parse_export_flags(cmd, 3, why);
    if (!flags) return error_answer(json, why);
    if (!flags->positionals.empty())
        return error_answer(json, std::format("unexpected argument \"{}\"", flags->positionals.front()));
    if (!flags->has("path") || !flags->has("fsid")) return error_answer(json, "add: --path and --fsid are required");
    CatalogExport exp;
    exp.cfg.path = flags->get("path");
    auto fsid = parse_uint(flags->get("fsid"), UINT32_MAX);
    if (!fsid || *fsid == 0)
        return error_answer(json, std::format("--fsid \"{}\": expected a non-zero number", flags->get("fsid")));
    exp.cfg.fsid = static_cast<uint32_t>(*fsid);
    if (flags->has("backend")) exp.cfg.backend = flags->get("backend");
    for (const auto& [k, v] : flags->opts) exp.cfg.backend_config.values[k] = v;
    if (auto ok = apply_dynamic_flags(*flags, exp, why); !ok) return error_answer(json, why);
    const bool force = flags->has("force") && flags->get("force") == "true";
    const bool dry_run = flags->has("dry-run") && flags->get("dry-run") == "true";
    if (!force)
        if (auto ok = check_fsid_reuse(*deps.store, exp, why); !ok) return error_answer(json, why);
    auto mutate = [&exp](const std::optional<Catalog>& current, std::string& why) -> Result<Catalog> {
        Catalog next = current ? *current : Catalog{};
        if (const auto* have = next.by_fsid(exp.cfg.fsid)) {
            why = std::format("fsid {} is already in the catalog ({}): use set, or remove it first", exp.cfg.fsid,
                              have->cfg.path);
            return Err(errno_from(EEXIST));
        }
        next.exports.push_back(exp);
        return next;
    };
    auto done = commit(deps, mutate, peer_uid, flags->has("comment") ? flags->get("comment") : "", dry_run, why);
    if (!done) return error_answer(json, "add failed: " + why);
    return commit_answer(*done, exp.cfg.fsid, dry_run ? "would be added" : "added", json);
}

std::string export_set(const CtlDeps& deps, const CtlCommand& cmd, std::optional<uint32_t> peer_uid, bool json) {
    std::string why;
    auto flags = parse_export_flags(cmd, 3, why);
    if (!flags) return error_answer(json, why);
    if (flags->positionals.empty()) return error_answer(json, "set: an fsid is required");
    auto fsid = parse_uint(flags->positionals.front(), UINT32_MAX);
    if (!fsid || *fsid == 0) return error_answer(json, std::format("bad fsid \"{}\"", flags->positionals.front()));
    if (flags->positionals.size() > 1)
        return error_answer(json, std::format("unexpected argument \"{}\"", flags->positionals[1]));
    if (flags->has("path") || flags->has("backend") || !flags->opts.empty())
        return error_answer(json, std::format("fsid {}: path, backend and backend keys cannot change: remove and "
                                              "re-add, or use a new fsid",
                                              *fsid));
    bool any = !flags->opts.empty();
    for (const auto& [name, unused] : flags->values)
        if (name != "comment" && name != "force" && name != "dry-run") any = true;
    if (!any) return error_answer(json, "set: nothing to change");
    const bool dry_run = flags->has("dry-run") && flags->get("dry-run") == "true";
    auto mutate = [&](const std::optional<Catalog>& current, std::string& why) -> Result<Catalog> {
        Catalog next = current ? *current : Catalog{};
        auto it = std::find_if(next.exports.begin(), next.exports.end(),
                               [&](const CatalogExport& e) { return e.cfg.fsid == *fsid; });
        if (it == next.exports.end()) {
            why = std::format("fsid {} is not in the catalog", *fsid);
            return Err(errno_from(ENOENT));
        }
        if (auto ok = apply_dynamic_flags(*flags, *it, why); !ok) return Err(ok.error());
        return next;
    };
    auto done = commit(deps, mutate, peer_uid, flags->has("comment") ? flags->get("comment") : "", dry_run, why);
    if (!done) return error_answer(json, "set failed: " + why);
    return commit_answer(*done, static_cast<uint32_t>(*fsid), dry_run ? "would be updated" : "updated", json);
}

std::string export_remove(const CtlDeps& deps, const CtlCommand& cmd, std::optional<uint32_t> peer_uid, bool json) {
    std::string why;
    auto flags = parse_export_flags(cmd, 3, why);
    if (!flags) return error_answer(json, why);
    if (flags->positionals.empty()) return error_answer(json, "remove: an fsid is required");
    auto fsid = parse_uint(flags->positionals.front(), UINT32_MAX);
    if (!fsid || *fsid == 0) return error_answer(json, std::format("bad fsid \"{}\"", flags->positionals.front()));
    for (const auto& [name, unused] : flags->values)
        if (name != "comment" && name != "force" && name != "dry-run")
            return error_answer(json, std::format("remove takes no --{}", name));
    const bool force = flags->has("force") && flags->get("force") == "true";
    const bool dry_run = flags->has("dry-run") && flags->get("dry-run") == "true";
    // The guard (design 11 §11.5): a served export is removed only on purpose.
    if (!force)
        if (std::string owner = served_by(deps, static_cast<uint32_t>(*fsid)); !owner.empty())
            return error_answer(json,
                                std::format("fsid {} is served by {} (disable it first, or --force)", *fsid, owner));
    auto mutate = [&](const std::optional<Catalog>& current, std::string& why) -> Result<Catalog> {
        Catalog next = current ? *current : Catalog{};
        auto it = std::find_if(next.exports.begin(), next.exports.end(),
                               [&](const CatalogExport& e) { return e.cfg.fsid == *fsid; });
        if (it == next.exports.end()) {
            why = std::format("fsid {} is not in the catalog", *fsid);
            return Err(errno_from(ENOENT));
        }
        next.exports.erase(it);
        return next;
    };
    auto done = commit(deps, mutate, peer_uid, flags->has("comment") ? flags->get("comment") : "", dry_run, why);
    if (!done) return error_answer(json, "remove failed: " + why);
    return commit_answer(*done, static_cast<uint32_t>(*fsid), dry_run ? "would be removed" : "removed", json);
}

const char* export_usage(bool json) {
    return json ? "{\"error\":\"bad subcommand\"}\n"
                : "cluster export: expected list|add --path P --fsid N [--backend B] [--nodes a,b] "
                  "[--clients c1,c2] [--readonly] [--squash root|all|none] [--anon-uid N] [--anon-gid N] "
                  "[--read-bps N] [--write-bps N] [--iops N] [--opt k=v …] [--disabled] [--force] "
                  "[--dry-run] [--comment T]|set <fsid> [--nodes …] [--clients …] [--readonly[=bool]] "
                  "[--squash …] [--anon-uid N] [--anon-gid N] [--read-bps N] [--write-bps N] [--iops N] "
                  "[--disabled[=bool]] [--dry-run] [--comment T]|remove <fsid> [--force] [--dry-run] "
                  "[--comment T]\n";
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

std::string cluster_export_answer(const CtlDeps& deps, const CtlCommand& cmd, std::optional<uint32_t> peer_uid) {
    const bool json = cmd.json;
    if (!deps.store) return json ? "{\"error\":\"not enabled\"}\n" : "cluster: not enabled\n";
    const auto sub = cmd.arg(2);
    if (sub == "list") return export_list(*deps.store, json);
    if (sub == "add") return export_add(deps, cmd, peer_uid, json);
    if (sub == "set") return export_set(deps, cmd, peer_uid, json);
    if (sub == "remove") return export_remove(deps, cmd, peer_uid, json);
    return export_usage(json);
}

}  // namespace lnfs::server
