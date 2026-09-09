// Shared export catalog (design 11, plan 12 A2): parse / serialize round trip, merge with
// the local per-node defaults, cluster-level validation, diff, and the local → catalog
// extraction used by `catalog import`.
#include "core/catalog.hpp"

#include <string>
#include <vector>

#include "mini_test.hpp"

using namespace lnfs;

namespace {

// Design 11 §11.2's example, one key per line (the local file's syntax).
const char* kSample = R"(
[catalog]
version    = 7
updated_at = "2026-09-08T10:21:03Z"
updated_by = "gw2 uid=0"
comment    = "add /export/c on gw3"

[[export]]
path = "/export/c"          # out of fsid order on purpose
fsid = 3
backend = "cephfs"
nodes = ["gw3", "gw1"]
clients = ["10.0.0.0/8"]
[export.cephfs]
fs_name = "cephfs"
subdir = "/nfs/c"

[[export]]
path = "/export/a/"
fsid = 1
backend = "cephfs"
nodes = ["gw1", "gw2", "gw3"]
readonly = true
squash = "all"
anon_uid = 1000
anon_gid = 1001
read_bps = "10MiB"
iops = 500
disabled = true
[export.cephfs]
fs_name = "cephfs"
subdir = "/nfs/a"
)";

core::CatalogExport make_export(uint32_t fsid, std::string path, std::string backend = "local",
                                std::vector<std::string> nodes = {"gw1", "gw2"}) {
    core::CatalogExport exp;
    exp.cfg.fsid = fsid;
    exp.cfg.path = std::move(path);
    exp.cfg.backend = std::move(backend);
    exp.cfg.nodes = std::move(nodes);
    return exp;
}

std::vector<uint32_t> ids(std::initializer_list<uint32_t> list) {
    return list;
}

}  // namespace

TEST(Catalog, ParseAndRoundTrip) {
    auto parsed = core::parse_catalog(kSample);
    ASSERT_TRUE(parsed.has_value());
    const core::Catalog& cat = *parsed;
    EXPECT_EQ(cat.meta.version, 7u);
    EXPECT_STREQ(cat.meta.updated_at, "2026-09-08T10:21:03Z");
    EXPECT_STREQ(cat.meta.updated_by, "gw2 uid=0");
    EXPECT_STREQ(cat.meta.comment, "add /export/c on gw3");
    ASSERT_TRUE(cat.exports.size() == 2u);
    // Sorted by fsid, path normalized, defaults where the file said nothing.
    EXPECT_EQ(cat.exports[0].cfg.fsid, 1u);
    EXPECT_STREQ(cat.exports[0].cfg.path, "/export/a");
    EXPECT_TRUE(cat.exports[0].disabled);
    EXPECT_TRUE(cat.exports[0].cfg.readonly);
    EXPECT_TRUE(cat.exports[0].cfg.squash == core::Squash::kAll);
    EXPECT_EQ(cat.exports[0].cfg.anon_uid, 1000u);
    EXPECT_EQ(cat.exports[0].cfg.read_bps, 10u << 20);
    EXPECT_EQ(cat.exports[0].cfg.iops, 500u);
    EXPECT_TRUE(cat.exports[0].cfg.clients == std::vector<std::string>({"0.0.0.0/0", "::/0"}));
    EXPECT_TRUE(cat.exports[0].cfg.nodes == std::vector<std::string>({"gw1", "gw2", "gw3"}));
    EXPECT_STREQ(cat.exports[0].cfg.backend_config.values.at("subdir"), "/nfs/a");
    EXPECT_EQ(cat.exports[1].cfg.fsid, 3u);
    EXPECT_FALSE(cat.exports[1].disabled);
    EXPECT_TRUE(cat.exports[1].cfg.clients == std::vector<std::string>({"10.0.0.0/8"}));
    EXPECT_TRUE(cat.by_fsid(3) == &cat.exports[1]);
    EXPECT_TRUE(cat.by_fsid(2) == nullptr);

    // parse → serialize → parse is the identity, and serialize is a fixed point.
    std::string text = core::serialize_catalog(cat);
    auto again = core::parse_catalog(text);
    ASSERT_TRUE(again.has_value());
    EXPECT_TRUE(*again == cat);
    EXPECT_STREQ(core::serialize_catalog(*again), text);
    // Canonical shape: header first, exports fsid ascending, every scalar written.
    EXPECT_TRUE(text.starts_with("[catalog]\nversion = 7\n"));
    EXPECT_TRUE(text.find("\n[[export]]\npath = \"/export/a\"\nfsid = 1\n") <
                text.find("\n[[export]]\npath = \"/export/c\"\nfsid = 3\n"));
    EXPECT_TRUE(text.find("disabled = true\n[export.cephfs]\nfs_name = \"cephfs\"\nsubdir = "
                          "\"/nfs/a\"\n") != std::string::npos);
    EXPECT_TRUE(text.find("read_bps = 10485760\n") != std::string::npos);

    // Escapes and value shapes survive: quotes / backslashes / newlines in strings, a
    // numeric-looking string vs a number, bare bools, an empty node list.
    core::Catalog odd;
    odd.meta.version = 1;
    odd.meta.comment = "say \"hi\"\\there\n\ttab";
    odd.meta.updated_by = "";
    auto e = make_export(9, "/x", "gluster", {});
    e.cfg.backend_config.values = {
        {"volume", "007"}, {"servers", "a,b"}, {"port", "24007"}, {"acl", "true"}, {"path with space", "v"}};
    e.cfg.clients = {"192.168.0.0/24", "fe80::/10"};
    odd.exports.push_back(e);
    auto odd_text = core::serialize_catalog(odd);
    EXPECT_TRUE(odd_text.find("volume = \"007\"\n") != std::string::npos);
    EXPECT_TRUE(odd_text.find("port = 24007\n") != std::string::npos);
    EXPECT_TRUE(odd_text.find("acl = true\n") != std::string::npos);
    EXPECT_TRUE(odd_text.find("nodes = []\n") != std::string::npos);
    auto odd_back = core::parse_catalog(odd_text);
    ASSERT_TRUE(odd_back.has_value());
    EXPECT_TRUE(*odd_back == odd);

    // An empty catalog (before the first publish) is a valid document too.
    core::Catalog empty;
    auto empty_back = core::parse_catalog(core::serialize_catalog(empty));
    ASSERT_TRUE(empty_back.has_value());
    EXPECT_TRUE(*empty_back == empty);
}

TEST(Catalog, PeekVersion) {
    // The store's poll reads only the header (plan 12 A3): no export parsing, so a
    // catalog with exports it cannot parse still answers its version.
    EXPECT_EQ(*core::peek_catalog_version(kSample), 7u);
    EXPECT_EQ(*core::peek_catalog_version("[catalog]\nversion=3\n"), 3u);
    EXPECT_EQ(*core::peek_catalog_version("# c\n\n[catalog]\ncomment = \"x\"\nversion = 12 # v\n"), 12u);
    EXPECT_EQ(*core::peek_catalog_version("[catalog]\nversion = 4\n[[export]]\nbogus = ??\n"), 4u);
    EXPECT_EQ(*core::peek_catalog_version(core::serialize_catalog(core::Catalog{})), 0u);
    auto bad = [](const char* text) { return !core::peek_catalog_version(text).has_value(); };
    EXPECT_TRUE(bad(""));
    EXPECT_TRUE(bad("[[export]]\nversion = 1\n"));  // wrong section
    EXPECT_TRUE(bad("[catalog]\ncomment = \"no version\"\n"));
    EXPECT_TRUE(bad("[catalog]\nversion = \"7\"\n"));
    EXPECT_TRUE(bad("version = 7\n[catalog]\n"));  // before the header
    EXPECT_TRUE(bad("[catalog]\n[[export]]\nversion = 7\n"));
}

TEST(Catalog, ParseRejects) {
    auto bad = [](const std::string& text) { return !core::parse_catalog(text).has_value(); };
    const std::string header = "[catalog]\nversion = 1\n";
    const std::string exp = "[[export]]\npath = \"/a\"\nfsid = 1\n";
    EXPECT_FALSE(bad(header + exp));
    EXPECT_TRUE(bad(exp));                                              // no [catalog] header
    EXPECT_TRUE(bad("[catalog]\ncomment = \"x\"\n" + exp));             // no version
    EXPECT_TRUE(bad(header + header + exp));                            // header twice
    EXPECT_TRUE(bad(header + "author = \"x\"\n" + exp));                // unknown header key
    EXPECT_TRUE(bad(header + "version = \"1\"\n"));                     // version must be a number
    EXPECT_TRUE(bad("version = 1\n" + exp));                            // key outside any section
    EXPECT_TRUE(bad(header + "[server]\nport = 1\n"));                  // local-only section
    EXPECT_TRUE(bad(header + exp + "sec = \"sys\"\n"));                 // unknown export key
    EXPECT_TRUE(bad(header + exp + "disabled = \"yes\"\n"));            // disabled is a bool
    EXPECT_TRUE(bad(header + "[export.local]\nhandles = \"auto\"\n"));  // subtable w/o export
    EXPECT_TRUE(bad(header + exp + "path\n"));                          // not key = value
    EXPECT_TRUE(bad(header + exp + "[export.local\n"));                 // unterminated header
    // Duplicate fsids parse (validate_catalog names them); the order is by fsid, stable.
    auto dup = core::parse_catalog(header + exp + exp);
    ASSERT_TRUE(dup.has_value());
    EXPECT_EQ(dup->exports.size(), 2u);
}

TEST(Catalog, PerNodeKeys) {
    // Reading: a per-node key in the catalog is dropped with a warning (forward compat).
    auto parsed = core::parse_catalog(
        "[catalog]\nversion = 2\n[[export]]\npath = \"/a\"\nfsid = 1\nbackend = \"cephfs\"\n"
        "[export.cephfs]\nfs_name = \"fs\"\nconf = \"/etc/ceph/ceph.conf\"\nkeyring = \"k\"\n");
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->exports.size() == 1u);
    const auto& values = parsed->exports[0].cfg.backend_config.values;
    EXPECT_EQ(values.size(), 1u);
    EXPECT_TRUE(values.contains("fs_name"));
    EXPECT_FALSE(values.contains("conf"));
    // Writing: validate_catalog refuses one, naming the key and the right place for it.
    core::Catalog cat;
    auto e = make_export(1, "/a", "cephfs");
    e.cfg.backend_config.values = {{"fs_name", "fs"}, {"conf", "/etc/ceph/ceph.conf"}};
    cat.exports.push_back(e);
    std::string why;
    EXPECT_FALSE(core::validate_catalog(cat, true, &why).has_value());
    EXPECT_TRUE(why.find("\"conf\" is a per-node key") != std::string::npos);
    EXPECT_TRUE(why.find("[backend_defaults.cephfs]") != std::string::npos);
    // Serializing never writes one either.
    EXPECT_TRUE(core::serialize_catalog(cat).find("conf") == std::string::npos);
}

TEST(Catalog, MergeWithLocal) {
    auto local = core::parse_config(
        "[cluster]\nenabled = true\nid = \"cluster-01\"\nshared_dir = \"/srv/shared\"\n"
        "exports_source = \"catalog\"\n"
        "[backend_defaults.cephfs]\nconf = \"/etc/ceph/ceph.conf\"\nkeyring = \"/etc/ceph/gw1\"\n"
        "fd_cache = 4096\n");
    ASSERT_TRUE(local.has_value());
    core::Catalog cat;
    auto ceph_b = make_export(2, "/b", "cephfs");
    ceph_b.cfg.backend_config.values = {{"fs_name", "fs"}, {"subdir", "/b"}};
    auto ceph_a = make_export(1, "/a", "cephfs");
    ceph_a.cfg.backend_config.values = {{"fs_name", "fs"}, {"conf", "/from/catalog"}};
    auto plain = make_export(3, "/tmp", "local");
    plain.cfg.backend_config.values = {{"handles", "auto"}};
    auto off = make_export(4, "/off", "cephfs");
    off.disabled = true;
    cat.exports = {ceph_b, ceph_a, plain, off};

    auto merged = core::merge_with_local(cat, *local);
    ASSERT_TRUE(merged.has_value());
    ASSERT_TRUE(merged->size() == 3u);  // the disabled export is not served
    EXPECT_EQ((*merged)[0].fsid, 1u);   // fsid ascending regardless of input order
    EXPECT_EQ((*merged)[1].fsid, 2u);
    EXPECT_EQ((*merged)[2].fsid, 3u);
    // Every cephfs export gets this host's keys; a per-node key smuggled into the catalog
    // never wins; the local export gets nothing (no [backend_defaults.local]).
    for (size_t i = 0; i < 2; ++i) {
        const auto& v = (*merged)[i].backend_config.values;
        EXPECT_STREQ(v.at("conf"), "/etc/ceph/ceph.conf");
        EXPECT_STREQ(v.at("keyring"), "/etc/ceph/gw1");
        EXPECT_STREQ(v.at("fd_cache"), "4096");
        EXPECT_STREQ(v.at("fs_name"), "fs");
    }
    EXPECT_EQ((*merged)[0].backend_config.values.size(), 4u);
    EXPECT_EQ((*merged)[1].backend_config.values.size(), 5u);
    EXPECT_EQ((*merged)[2].backend_config.values.size(), 1u);
    EXPECT_STREQ((*merged)[2].backend_config.values.at("handles"), "auto");
    EXPECT_TRUE((*merged)[2].nodes == std::vector<std::string>({"gw1", "gw2"}));
    // Nothing to merge is fine (a host without defaults for that backend).
    core::Config bare;
    EXPECT_TRUE(core::merge_with_local(cat, bare)->size() == 3u);
    // A defaults table built in memory with a cluster-wide key is refused.
    core::Config broken = *local;
    broken.backend_defaults["cephfs"].values["fs_name"] = "other";
    EXPECT_FALSE(core::merge_with_local(cat, broken).has_value());
}

TEST(Catalog, ValidateRules) {
    auto sample = core::parse_catalog(kSample);
    ASSERT_TRUE(sample.has_value());
    EXPECT_TRUE(core::validate_catalog(*sample, true).has_value());
    EXPECT_TRUE(core::validate_catalog(*sample, false).has_value());
    core::Catalog empty;
    EXPECT_TRUE(core::validate_catalog(empty, true).has_value());

    auto reason = [](const core::Catalog& cat, bool aa, int err = EINVAL) {
        std::string why;
        auto r = core::validate_catalog(cat, aa, &why);
        if (r.has_value()) return std::string("(accepted)");
        if (r.error() != errno_from(err)) return std::string("(wrong errno)");
        return why;
    };
    auto contains = [](const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; };
    core::Catalog cat;
    cat.exports = {make_export(1, "/a"), make_export(2, "/b")};
    EXPECT_STREQ(reason(cat, true), "(accepted)");

    auto with = [&](core::CatalogExport extra) {
        core::Catalog c = cat;
        c.exports.push_back(std::move(extra));
        return c;
    };
    EXPECT_TRUE(contains(reason(with(make_export(0, "/z")), true), "fsid must be non-zero"));
    EXPECT_TRUE(contains(reason(with(make_export(2, "/z")), true), "fsid=2: fsid is listed twice"));
    EXPECT_TRUE(contains(reason(with(make_export(3, "")), true), "path must be absolute"));
    EXPECT_TRUE(contains(reason(with(make_export(3, "rel")), true), "path must be absolute"));
    EXPECT_TRUE(contains(reason(with(make_export(3, "/z/")), true), "no trailing slash"));
    EXPECT_TRUE(contains(reason(with(make_export(3, "/a")), true), "fsid=1 and fsid=3 share path /a"));
    EXPECT_TRUE(contains(reason(with(make_export(3, "/a/sub")), true), "fsid=3 (/a/sub) is nested in fsid=1 (/a)"));
    EXPECT_TRUE(contains(reason(with(make_export(3, "/")), true), "fsid=1 (/a) is nested in fsid=3 (/)"));
    EXPECT_STREQ(reason(with(make_export(3, "/ab")), true), "(accepted)");  // not a prefix
    EXPECT_TRUE(contains(reason(with(make_export(3, "/z", "bogus")), true, ENODEV), "no such backend \"bogus\""));
    EXPECT_TRUE(
        contains(reason(with(make_export(3, "/z", "local", {})), true), "nodes is required under active-active"));
    EXPECT_STREQ(reason(with(make_export(3, "/z", "local", {})), false), "(accepted)");
    EXPECT_TRUE(contains(reason(with(make_export(3, "/z", "local", {"gw1", "gw1"})), true),
                         "invalid or duplicate entry \"gw1\""));
    EXPECT_TRUE(contains(reason(with(make_export(3, "/z", "local", {"bad name"})), false),
                         "invalid or duplicate entry \"bad name\""));
    auto no_clients = make_export(3, "/z");
    no_clients.cfg.clients.clear();
    EXPECT_TRUE(contains(reason(with(no_clients), true), "clients must not be empty"));
    auto bad_cidr = make_export(3, "/z");
    bad_cidr.cfg.clients = {"10.0.0.0/33"};
    EXPECT_TRUE(contains(reason(with(bad_cidr), true), "bad client CIDR \"10.0.0.0/33\""));
    // A disabled export still occupies its fsid and path.
    auto off = make_export(1, "/off");
    off.disabled = true;
    EXPECT_TRUE(contains(reason(with(off), true), "fsid is listed twice"));
    // Same Gluster volume ⇒ same nodes (design 10 §10.6); only an active-active rule.
    auto g1 = make_export(3, "/g1", "gluster", {"gw1", "gw2"});
    g1.cfg.backend_config.values = {{"volume", "vol"}};
    auto g2 = make_export(4, "/g2", "gluster", {"gw2", "gw1"});
    g2.cfg.backend_config.values = {{"volume", "vol"}};
    auto both = with(g1);
    both.exports.push_back(g2);
    EXPECT_TRUE(contains(reason(both, true), "fsid=3 and fsid=4 share one gluster volume"));
    EXPECT_STREQ(reason(both, false), "(accepted)");
    both.exports.back().cfg.nodes = g1.cfg.nodes;
    EXPECT_STREQ(reason(both, true), "(accepted)");
    both.exports.back().cfg.nodes = g2.cfg.nodes;
    both.exports.back().cfg.backend_config.values["volume"] = "other";
    EXPECT_STREQ(reason(both, true), "(accepted)");
    // Without a `why` slot the reason goes to the log and only the errno comes back.
    EXPECT_FALSE(core::validate_catalog(with(make_export(0, "/z")), true).has_value());
}

TEST(Catalog, Diff) {
    core::Catalog from;
    for (uint32_t fsid = 1; fsid <= 7; ++fsid) {
        auto e = make_export(fsid, "/e" + std::to_string(fsid), "cephfs");
        e.cfg.backend_config.values = {{"fs_name", "fs"}, {"subdir", "/e" + std::to_string(fsid)}};
        from.exports.push_back(e);
    }
    from.exports[2].disabled = true;  // fsid 3
    core::Catalog to = from;
    to.meta.version = from.meta.version + 1;
    // 1: nodes and clients changed; 2: removed; 3: enabled (and nodes changed); 4: disabled;
    // 5: path changed (rejected, even though nodes changed too); 6: cluster key changed
    // (rejected); 7: only a per-node key differs (nothing); 8: added; 9: added disabled.
    to.exports[0].cfg.nodes = {"gw2"};
    to.exports[0].cfg.clients = {"10.0.0.0/8"};
    to.exports.erase(to.exports.begin() + 1);
    auto at = [&](uint32_t fsid) -> core::CatalogExport& {
        for (auto& e : to.exports)
            if (e.cfg.fsid == fsid) return e;
        std::abort();
    };
    at(3).disabled = false;
    at(3).cfg.nodes = {"gw3"};
    at(4).disabled = true;
    at(5).cfg.path = "/moved";
    at(5).cfg.nodes = {"gw3"};
    at(6).cfg.backend_config.values["subdir"] = "/elsewhere";
    at(7).cfg.backend_config.values["conf"] = "/host/only";
    to.exports.push_back(make_export(9, "/e9"));
    to.exports.back().disabled = true;
    to.exports.push_back(make_export(8, "/e8"));

    core::CatalogDiff diff = core::diff_catalog(from, to);
    EXPECT_TRUE(diff.added == ids({8, 9}));
    EXPECT_TRUE(diff.removed == ids({2}));
    EXPECT_TRUE(diff.disabled == ids({4}));
    EXPECT_TRUE(diff.enabled == ids({3}));
    EXPECT_TRUE(diff.nodes_changed == ids({1, 3}));
    EXPECT_TRUE(diff.dynamic_changed == ids({1}));
    EXPECT_TRUE(diff.rejected == ids({5, 6}));
    EXPECT_FALSE(diff.empty());
    // Each dynamic field counts; identity and header changes alone are nothing.
    core::Catalog same = from;
    same.meta.version = 99;
    EXPECT_TRUE(core::diff_catalog(from, same).empty());
    for (auto mutate : {+[](core::ExportConfig& c) { c.readonly = !c.readonly; },
                        +[](core::ExportConfig& c) { c.squash = core::Squash::kNone; },
                        +[](core::ExportConfig& c) { c.anon_uid = 1; }, +[](core::ExportConfig& c) { c.anon_gid = 1; },
                        +[](core::ExportConfig& c) { c.read_bps = 1; }, +[](core::ExportConfig& c) { c.write_bps = 1; },
                        +[](core::ExportConfig& c) { c.iops = 1; }}) {
        core::Catalog one = from;
        mutate(one.exports[0].cfg);
        EXPECT_TRUE(core::diff_catalog(from, one).dynamic_changed == ids({1}));
    }
    // A backend swap is identity too.
    core::Catalog swapped = from;
    swapped.exports[0].cfg.backend = "local";
    EXPECT_TRUE(core::diff_catalog(from, swapped).rejected == ids({1}));
}

TEST(Catalog, FromConfig) {
    auto local = core::parse_config(
        "[cluster]\nenabled = true\nid = \"cluster-01\"\nshared_dir = \"/srv/shared\"\n"
        "mode = \"active-active\"\nnode = \"gw1\"\nnode_address = \"10.0.0.11:2049\"\n"
        "[[export]]\npath = \"/export/b\"\nfsid = 2\nbackend = \"cephfs\"\nnodes = [\"gw2\"]\n"
        "clients = [\"10.0.0.0/8\"]\nread_bps = \"1MiB\"\n"
        "[export.cephfs]\nfs_name = \"fs\"\nsubdir = \"/b\"\nconf = \"/etc/ceph/ceph.conf\"\n"
        "keyring = \"/etc/ceph/gw1\"\nname = \"client.gw1\"\n"
        "[[export]]\npath = \"/export/a\"\nfsid = 1\nbackend = \"cephfs\"\nnodes = [\"gw1\"]\n"
        "readonly = true\n[export.cephfs]\nfs_name = \"fs\"\nconf = \"/etc/ceph/ceph.conf\"\n"
        "keyring = \"/etc/ceph/gw1\"\nname = \"client.gw1\"\n");
    ASSERT_TRUE(local.has_value());
    core::Catalog cat = core::catalog_from_config(*local);
    EXPECT_EQ(cat.meta.version, 0u);
    ASSERT_TRUE(cat.exports.size() == 2u);
    EXPECT_EQ(cat.exports[0].cfg.fsid, 1u);
    EXPECT_EQ(cat.exports[1].cfg.fsid, 2u);
    for (const auto& e : cat.exports) {
        EXPECT_FALSE(e.disabled);
        EXPECT_FALSE(e.cfg.backend_config.values.contains("conf"));
        EXPECT_FALSE(e.cfg.backend_config.values.contains("keyring"));
        EXPECT_FALSE(e.cfg.backend_config.values.contains("name"));
        EXPECT_TRUE(e.cfg.backend_config.values.contains("fs_name"));
    }
    EXPECT_EQ(cat.exports[1].cfg.read_bps, 1u << 20);
    EXPECT_TRUE(cat.exports[1].cfg.clients == std::vector<std::string>({"10.0.0.0/8"}));
    EXPECT_TRUE(core::validate_catalog(cat, true).has_value());
    // The extracted catalog survives the text form.
    auto back = core::parse_catalog(core::serialize_catalog(cat));
    ASSERT_TRUE(back.has_value());
    EXPECT_TRUE(*back == cat);

    // §11.9: a gateway switched to catalog mode, with the per-node keys moved to
    // [backend_defaults.cephfs], computes the same export digest as the local-mode one.
    auto switched = core::parse_config(
        "[cluster]\nenabled = true\nid = \"cluster-01\"\nshared_dir = \"/srv/shared\"\n"
        "mode = \"active-active\"\nnode = \"gw1\"\nnode_address = \"10.0.0.11:2049\"\n"
        "exports_source = \"catalog\"\n[backend_defaults.cephfs]\nconf = \"/etc/ceph/ceph.conf\"\n"
        "keyring = \"/etc/ceph/gw1\"\nname = \"client.gw1\"\n");
    ASSERT_TRUE(switched.has_value());
    auto merged = core::merge_with_local(*back, *switched);
    ASSERT_TRUE(merged.has_value());
    core::Config effective = *switched;
    effective.exports = *merged;
    EXPECT_STREQ(core::canonical_exports_text(effective), core::canonical_exports_text(*local));
    EXPECT_STREQ(core::canonical_exports_digest(effective), core::canonical_exports_digest(*local));
    // And the merged exports are, field for field, the local ones (fsid order aside).
    EXPECT_TRUE((*merged)[0] == local->exports[1]);
    EXPECT_TRUE((*merged)[1] == local->exports[0]);
}

TEST(Catalog, LocalParserSharesTheExportSyntax) {
    // The local file goes through the same export-block parser: unknown keys are now
    // EINVAL (they used to be ignored), `disabled` stays catalog-only, and a subtable
    // before any [[export]] is still refused.
    const std::string exp = "[[export]]\npath = \"/tmp\"\nfsid = 1\n";
    EXPECT_TRUE(core::parse_config(exp + "[export.local]\nhandles = \"auto\"\n[server]\nport = 1\n").has_value());
    EXPECT_FALSE(core::parse_config(exp + "sec = \"sys\"\n").has_value());
    EXPECT_FALSE(core::parse_config(exp + "disabled = true\n").has_value());
    EXPECT_FALSE(core::parse_config("[export.local]\nhandles = \"auto\"\n" + exp).has_value());
    // A second [[export]] closes the first block's subtable.
    auto two = core::parse_config(exp + "[export.local]\nhandles = \"auto\"\n" + exp +
                                  "[export.local]\nhandles = \"inode\"\n");
    ASSERT_TRUE(two.has_value());
    ASSERT_TRUE(two->exports.size() == 2u);
    EXPECT_STREQ(two->exports[0].backend_config.values.at("handles"), "auto");
    EXPECT_STREQ(two->exports[1].backend_config.values.at("handles"), "inode");
}
