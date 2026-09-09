// Export-set snapshots (plan 12 B1): the export table publishes immutable ExportSet
// versions; a reader's snapshot is stable while later publishes swap the table's
// current set, entries are shared across versions, and the pseudo tree travels with
// the set.

#include <cerrno>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "backend/memory/memory.hpp"
#include "core/config.hpp"
#include "core/pseudofs.hpp"
#include "mini_test.hpp"

using namespace lnfs;

namespace {

core::ExportConfig export_cfg(uint32_t fsid, std::string path) {
    core::ExportConfig cfg;
    cfg.path = std::move(path);
    cfg.fsid = fsid;
    cfg.clients = {"127.0.0.0/8"};
    return cfg;
}

std::unique_ptr<backend::Backend> mem(uint32_t fsid) {
    return std::make_unique<backend::MemoryBackend>(fsid);
}

std::vector<uint32_t> fsids(const core::ExportSet& set) {
    std::vector<uint32_t> out;
    for (const auto& entry : set.entries) out.push_back(entry->fsid);
    return out;
}

}  // namespace

TEST(ExportSet, EmptyTableHasAPseudoRoot) {
    core::ExportTable table;
    auto set = table.snapshot();
    ASSERT_TRUE(set != nullptr);
    EXPECT_EQ(set->generation, 0u);
    EXPECT_EQ(set->epoch, 1u);
    EXPECT_TRUE(set->entries.empty());
    ASSERT_TRUE(set->pseudo != nullptr);
    EXPECT_TRUE(set->pseudo->root()->exp == nullptr);
    EXPECT_TRUE(set->by_fsid(1) == nullptr);
    std::string rel;
    EXPECT_TRUE(set->for_mount_path("/anything", rel) == nullptr);
    EXPECT_EQ(table.size(), 0u);
}

TEST(ExportSet, SnapshotIsStable) {
    core::ExportTable table;
    ASSERT_TRUE(table.add(export_cfg(2, "/export/two"), mem(2)).has_value());
    ASSERT_TRUE(table.add(export_cfg(1, "/export/one"), mem(1)).has_value());
    auto before = table.snapshot();
    const core::ExportEntry* one = before->by_fsid(1);
    ASSERT_TRUE(one != nullptr);
    EXPECT_TRUE(before->by_fsid(3) == nullptr);
    ASSERT_TRUE(before->pseudo->for_export(1) != nullptr);
    EXPECT_TRUE(before->pseudo->for_export(3) == nullptr);
    const uint64_t generation = before->generation;

    // A publish that drops fsid 1 and adds fsid 3 (what B2's apply() will do).
    core::ExportSetBuilder builder;
    ASSERT_TRUE(builder.add(export_cfg(3, "/export/three"), mem(3)).has_value());
    table.publish_for_test(builder.finish(before->epoch, generation + 1));

    // The snapshot taken earlier still answers as it did.
    EXPECT_TRUE(before->by_fsid(1) == one);
    EXPECT_TRUE(before->by_fsid(3) == nullptr);
    EXPECT_TRUE(before->pseudo->for_export(1) != nullptr);
    EXPECT_EQ(before->generation, generation);
    EXPECT_STREQ(one->path, "/export/one");

    // The table's current set is the new one.
    auto after = table.snapshot();
    EXPECT_TRUE(after != before);
    EXPECT_EQ(after->generation, generation + 1);
    EXPECT_TRUE(after->by_fsid(1) == nullptr);
    EXPECT_TRUE(after->by_fsid(3) != nullptr);
    EXPECT_TRUE(after->pseudo->for_export(3) != nullptr);
    EXPECT_TRUE(after->pseudo->for_export(1) == nullptr);
    EXPECT_TRUE(table.by_fsid(3) == after->by_fsid(3));
    EXPECT_EQ(table.size(), 1u);

    // Dropping the old snapshot frees the retired version; the new one is unaffected.
    before.reset();
    one = nullptr;
    EXPECT_TRUE(table.snapshot()->by_fsid(3) != nullptr);
}

TEST(ExportSet, EntriesSortedByFsidAndSharedAcrossPublishes) {
    core::ExportTable table;
    ASSERT_TRUE(table.add(export_cfg(5, "/e/five"), mem(5)).has_value());
    ASSERT_TRUE(table.add(export_cfg(2, "/e/two"), mem(2)).has_value());
    ASSERT_TRUE(table.add(export_cfg(9, "/e/nine"), mem(9)).has_value());
    auto first = table.snapshot();
    EXPECT_TRUE(fsids(*first) == (std::vector<uint32_t>{2, 5, 9}));
    // one publish per add on top of the empty set
    EXPECT_EQ(first->generation, 3u);
    for (uint32_t fsid : {2u, 5u, 9u}) {
        const auto* entry = first->by_fsid(fsid);
        ASSERT_TRUE(entry != nullptr);
        EXPECT_EQ(entry->fsid, fsid);
        EXPECT_TRUE(first->pseudo->for_export(fsid) != nullptr);
        EXPECT_TRUE(first->pseudo->for_export(fsid)->exp == entry);
    }

    // add() publishes a superset that shares the existing entry objects.
    ASSERT_TRUE(table.add(export_cfg(7, "/e/seven"), mem(7)).has_value());
    auto second = table.snapshot();
    EXPECT_TRUE(fsids(*second) == (std::vector<uint32_t>{2, 5, 7, 9}));
    EXPECT_EQ(second->generation, 4u);
    for (uint32_t fsid : {2u, 5u, 9u}) {
        EXPECT_TRUE(second->by_fsid(fsid) == first->by_fsid(fsid));
        EXPECT_EQ(second->entries[fsid == 2 ? 0 : fsid == 5 ? 1 : 3].use_count(), 2);
    }
    EXPECT_TRUE(first->by_fsid(7) == nullptr);
    EXPECT_TRUE(second->by_fsid(7) != nullptr);
    // The new set has its own pseudo tree; node ids are path-derived, so the shared
    // exports keep their ids.
    EXPECT_TRUE(second->pseudo.get() != first->pseudo.get());
    EXPECT_EQ(second->pseudo->for_export(5)->id, first->pseudo->for_export(5)->id);
}

TEST(ExportSet, BuilderRejectsBadEntries) {
    core::ExportSetBuilder builder;
    EXPECT_FALSE(builder.add(export_cfg(0, "/zero"), mem(0)).has_value());
    EXPECT_FALSE(builder.add(export_cfg(4, "/nobackend"), nullptr).has_value());
    ASSERT_TRUE(builder.add(export_cfg(4, "/four"), mem(4)).has_value());
    EXPECT_FALSE(builder.add(export_cfg(4, "/four-again"), mem(4)).has_value());
    auto bad_client = export_cfg(6, "/six");
    bad_client.clients = {"not-a-cidr"};
    EXPECT_FALSE(builder.add(bad_client, mem(6)).has_value());
    auto set = builder.finish(11, 3);
    EXPECT_TRUE(fsids(*set) == (std::vector<uint32_t>{4}));
    EXPECT_EQ(set->epoch, 11u);
    EXPECT_EQ(set->generation, 3u);

    // The table refuses the same things, and a refused add publishes nothing.
    core::ExportTable table(set);
    auto same = table.snapshot();
    EXPECT_TRUE(same == set);
    EXPECT_FALSE(table.add(export_cfg(4, "/dup"), mem(4)).has_value());
    EXPECT_FALSE(table.add(export_cfg(0, "/zero"), mem(0)).has_value());
    EXPECT_TRUE(table.snapshot() == set);

    // Building from an existing set shares its entries.
    core::ExportSetBuilder next(*set);
    ASSERT_TRUE(next.add(export_cfg(8, "/eight"), mem(8)).has_value());
    auto grown = next.finish(11, 4);
    EXPECT_TRUE(grown->by_fsid(4) == set->by_fsid(4));
    EXPECT_TRUE(fsids(*grown) == (std::vector<uint32_t>{4, 8}));
}

TEST(ExportSet, ForMountPathLongestPrefixOnComponentBoundary) {
    core::ExportTable table;
    ASSERT_TRUE(table.add(export_cfg(1, "/export"), mem(1)).has_value());
    ASSERT_TRUE(table.add(export_cfg(2, "/export/deep"), mem(2)).has_value());
    auto set = table.snapshot();
    std::string rel;
    const auto* hit = set->for_mount_path("/export/deep/a/b", rel);
    ASSERT_TRUE(hit != nullptr);
    EXPECT_EQ(hit->fsid, 2u);
    EXPECT_STREQ(rel, "a/b");
    // not a component boundary
    hit = set->for_mount_path("/export/deeper", rel);
    ASSERT_TRUE(hit != nullptr);
    EXPECT_EQ(hit->fsid, 1u);
    EXPECT_STREQ(rel, "deeper");
    hit = set->for_mount_path("/export/", rel);
    ASSERT_TRUE(hit != nullptr);
    EXPECT_EQ(hit->fsid, 1u);
    EXPECT_STREQ(rel, "");
    EXPECT_TRUE(set->for_mount_path("/exports", rel) == nullptr);
}

TEST(ExportSet, SetEpochRepublishesThePseudoTree) {
    core::ExportTable table;
    ASSERT_TRUE(table.add(export_cfg(1, "/export/data"), mem(1)).has_value());
    auto boot = table.snapshot();
    EXPECT_EQ(boot->epoch, 1u);
    EXPECT_EQ(boot->pseudo->attr_of(*boot->pseudo->root()).change, boot->pseudo_change());

    table.set_epoch(42);
    auto served = table.snapshot();
    EXPECT_TRUE(served != boot);
    EXPECT_EQ(served->epoch, 42u);
    EXPECT_EQ(served->generation, boot->generation + 1);
    EXPECT_EQ(served->pseudo_change(), (42ull << 32) | served->generation);
    EXPECT_EQ(served->pseudo->attr_of(*served->pseudo->root()).change, served->pseudo_change());
    EXPECT_EQ(served->pseudo->attr_of(*served->pseudo->for_export(1)).change, served->pseudo_change());
    // Same entries, same pseudo ids; the earlier snapshot keeps its own change base.
    EXPECT_TRUE(served->by_fsid(1) == boot->by_fsid(1));
    EXPECT_EQ(served->pseudo->for_export(1)->id, boot->pseudo->for_export(1)->id);
    EXPECT_EQ(boot->pseudo->attr_of(*boot->pseudo->root()).change, boot->pseudo_change());
    // Re-applying the same epoch publishes nothing.
    table.set_epoch(42);
    EXPECT_TRUE(table.snapshot() == served);
    // A later add keeps the epoch.
    ASSERT_TRUE(table.add(export_cfg(2, "/export/more"), mem(2)).has_value());
    EXPECT_EQ(table.snapshot()->epoch, 42u);
}

TEST(ExportSet, ReloadDynamicUpdatesEntriesInPlace) {
    core::ExportTable table;
    auto cfg = export_cfg(1, "/export/data");
    cfg.iops = 10;
    ASSERT_TRUE(table.add(cfg, mem(1)).has_value());
    auto before = table.snapshot();
    const auto* entry = before->by_fsid(1);
    ASSERT_TRUE(entry != nullptr);
    EXPECT_EQ(entry->client_list().size(), 1u);
    EXPECT_EQ(entry->qos.ops.rate(), 10u);

    core::Config fresh;
    fresh.exports.push_back(export_cfg(1, "/export/data"));
    fresh.exports.back().clients = {"127.0.0.0/8", "10.0.0.0/8"};
    fresh.exports.back().iops = 25;
    std::string report = table.reload_dynamic(fresh);
    EXPECT_TRUE(report.find("clients (2) and qos applied") != std::string::npos);
    // No publish: the snapshot is the same object and its entry changed in place.
    EXPECT_TRUE(table.snapshot() == before);
    EXPECT_EQ(entry->client_list().size(), 2u);
    EXPECT_EQ(entry->qos.ops.rate(), 25u);
}

// ---- plan 12 B2: apply / in-place update / retirement ---------------------------

namespace {

std::vector<std::unique_ptr<backend::Backend>> backends(std::initializer_list<uint32_t> fsids) {
    std::vector<std::unique_ptr<backend::Backend>> out;
    for (uint32_t fsid : fsids) out.push_back(mem(fsid));
    return out;
}

}  // namespace

TEST(ExportSet, ApplyAddsRemovesAndSharesEntries) {
    core::ExportTable table;
    ASSERT_TRUE(table.add(export_cfg(1, "/export/one"), mem(1)).has_value());
    ASSERT_TRUE(table.add(export_cfg(2, "/export/two"), mem(2)).has_value());
    ASSERT_TRUE(table.add(export_cfg(3, "/export/three"), mem(3)).has_value());
    auto old = table.snapshot();
    const core::ExportEntry* one = old->by_fsid(1);
    const core::ExportEntry* two = old->by_fsid(2);
    const core::ExportEntry* three = old->by_fsid(3);

    // Add fsid 4, remove fsid 2.
    core::ExportSetPlan plan;
    plan.add.push_back(export_cfg(4, "/export/four"));
    plan.remove.push_back(2);
    auto started = backends({4});
    backend::Backend* four_backend = started[0].get();
    auto applied = table.apply(std::move(plan), started, old->epoch);
    ASSERT_TRUE(applied.has_value());
    // consumed
    EXPECT_TRUE(started.empty());
    // the only reference to the new set besides the table
    auto next = std::move(*applied);
    EXPECT_TRUE(table.snapshot() == next);
    EXPECT_EQ(next->generation, old->generation + 1);
    EXPECT_TRUE(fsids(*next) == (std::vector<uint32_t>{1, 3, 4}));
    // Unchanged exports are the same objects; the new one carries the started backend.
    EXPECT_TRUE(next->by_fsid(1) == one);
    EXPECT_TRUE(next->by_fsid(3) == three);
    EXPECT_TRUE(next->by_fsid(2) == nullptr);
    ASSERT_TRUE(next->by_fsid(4) != nullptr);
    EXPECT_TRUE(next->by_fsid(4)->backend.get() == four_backend);
    EXPECT_STREQ(next->by_fsid(4)->path, "/export/four");
    // The new pseudo tree lists the new export and not the removed one; the old
    // snapshot is untouched.
    EXPECT_TRUE(next->pseudo->for_export(4) != nullptr);
    EXPECT_TRUE(next->pseudo->for_export(2) == nullptr);
    EXPECT_TRUE(old->pseudo->for_export(2) != nullptr);
    EXPECT_TRUE(old->by_fsid(2) == two);
    EXPECT_TRUE(fsids(*old) == (std::vector<uint32_t>{1, 2, 3}));

    // The removed entry is retired but still referenced by the old snapshot: nothing to
    // take yet.
    EXPECT_EQ(table.retired_pending(), 1u);
    EXPECT_TRUE(table.oldest_retired().has_value());
    EXPECT_TRUE(table.take_retired().empty());
    EXPECT_EQ(table.retired_pending(), 1u);
    // Once the old snapshot is gone the entry is handed out exactly once.
    old.reset();
    auto retired = table.take_retired();
    ASSERT_TRUE(retired.size() == 1u);
    EXPECT_TRUE(retired[0].get() == two);
    EXPECT_EQ(retired[0]->fsid, 2u);
    EXPECT_EQ(retired[0].use_count(), 1);
    EXPECT_EQ(table.retired_pending(), 0u);
    EXPECT_FALSE(table.oldest_retired().has_value());
    EXPECT_TRUE(table.take_retired().empty());

    // Replacing an fsid in one plan (remove + add) hands the old entry to the queue and
    // installs the new one.
    core::ExportSetPlan swap;
    swap.remove.push_back(4);
    swap.add.push_back(export_cfg(4, "/export/four-again"));
    auto swap_backends = backends({4});
    ASSERT_TRUE(table.apply(std::move(swap), swap_backends, 1).has_value());
    next.reset();
    ASSERT_TRUE(table.snapshot()->by_fsid(4) != nullptr);
    EXPECT_STREQ(table.snapshot()->by_fsid(4)->path, "/export/four-again");
    EXPECT_TRUE(table.snapshot()->by_fsid(4)->backend.get() != four_backend);
    auto swapped = table.take_retired();
    ASSERT_TRUE(swapped.size() == 1u);
    EXPECT_TRUE(swapped[0]->backend.get() == four_backend);
}

TEST(ExportSet, PseudoChangeMonotonic) {
    core::ExportTable table;
    std::vector<uint64_t> seen;
    auto note = [&] {
        auto set = table.snapshot();
        uint64_t change = set->pseudo->attr_of(*set->pseudo->root()).change;
        EXPECT_EQ(change, set->pseudo_change());
        if (!seen.empty()) EXPECT_TRUE(change > seen.back());
        seen.push_back(change);
    };
    // the empty boot set
    note();
    ASSERT_TRUE(table.add(export_cfg(1, "/export/one"), mem(1)).has_value());
    note();
    // what the ProtocolStack does when the gateway activates
    table.set_epoch(5);
    note();
    core::ExportSetPlan add;
    add.add.push_back(export_cfg(2, "/export/two"));
    auto started = backends({2});
    ASSERT_TRUE(table.apply(std::move(add), started, 5).has_value());
    note();
    // an in-place update publishes too: the tree moves
    core::ExportSetPlan update;
    update.update.push_back(export_cfg(2, "/export/two"));
    std::vector<std::unique_ptr<backend::Backend>> none;
    ASSERT_TRUE(table.apply(std::move(update), none, 5).has_value());
    note();
    core::ExportSetPlan remove;
    remove.remove.push_back(1);
    ASSERT_TRUE(table.apply(std::move(remove), none, 5).has_value());
    note();
    // A restart: the next epoch with generation back at 0 still sorts after every
    // version of the previous incarnation.
    core::ExportSetBuilder rebooted;
    ASSERT_TRUE(rebooted.add(export_cfg(2, "/export/two"), mem(2)).has_value());
    auto fresh = rebooted.finish(6, 0);
    EXPECT_TRUE(fresh->pseudo_change() > seen.back());
    EXPECT_EQ(fresh->pseudo->attr_of(*fresh->pseudo->root()).change, 6ull << 32);
    // Only the export's own pseudo node and its parents move; export attributes come
    // from the backend and are not touched.
    EXPECT_EQ(seen.size(), 6u);
}

TEST(ExportSet, DynamicFieldsUpdateInPlace) {
    core::ExportTable table;
    auto cfg = export_cfg(1, "/export/data");
    cfg.squash = core::Squash::kRoot;
    cfg.iops = 10;
    cfg.nodes = {"gw1", "gw2"};
    ASSERT_TRUE(table.add(cfg, mem(1)).has_value());
    auto old = table.snapshot();
    core::ExportEntry* entry = old->by_fsid(1);
    ASSERT_TRUE(entry != nullptr);
    EXPECT_FALSE(entry->readonly.load());
    EXPECT_TRUE(entry->squash.load() == core::Squash::kRoot);
    EXPECT_EQ(entry->anon_uid.load(), 65534u);
    EXPECT_EQ(entry->node_list().size(), 2u);
    const auto* nodes_before = &entry->node_list();
    const auto* clients_before = &entry->client_list();
    rpc::Cred root_cred;
    root_cred.uid = 0;
    root_cred.gid = 0;
    EXPECT_EQ(core::ExportTable::squash_cred(root_cred, *entry).uid, 65534u);

    core::ExportSetPlan plan;
    auto fresh = export_cfg(1, "/export/data");
    fresh.readonly = true;
    fresh.squash = core::Squash::kAll;
    fresh.anon_uid = 1000;
    fresh.anon_gid = 1001;
    fresh.clients = {"127.0.0.0/8", "10.0.0.0/8"};
    fresh.iops = 25;
    fresh.nodes = {"gw3"};
    plan.update.push_back(fresh);
    std::vector<std::unique_ptr<backend::Backend>> none;
    auto applied = table.apply(std::move(plan), none, old->epoch);
    ASSERT_TRUE(applied.has_value());
    // The same entry object, in both the old and the new set, reads the new values.
    EXPECT_TRUE((*applied)->by_fsid(1) == entry);
    EXPECT_TRUE(old->by_fsid(1) == entry);
    EXPECT_TRUE(entry->readonly.load());
    EXPECT_TRUE(entry->squash.load() == core::Squash::kAll);
    EXPECT_EQ(entry->anon_uid.load(), 1000u);
    EXPECT_EQ(entry->anon_gid.load(), 1001u);
    EXPECT_EQ(entry->qos.ops.rate(), 25u);
    EXPECT_EQ(entry->client_list().size(), 2u);
    EXPECT_TRUE(&entry->client_list() != clients_before);
    ASSERT_TRUE(entry->node_list().size() == 1u);
    EXPECT_STREQ(entry->node_list()[0], "gw3");
    EXPECT_TRUE(&entry->node_list() != nodes_before);
    // A reader that loaded the old list pointer still sees a valid (retired) list.
    EXPECT_EQ(nodes_before->size(), 2u);
    rpc::Cred user_cred;
    user_cred.uid = 500;
    user_cred.gid = 500;
    auto mapped = core::ExportTable::squash_cred(user_cred, *entry);
    EXPECT_EQ(mapped.uid, 1000u);
    EXPECT_EQ(mapped.gid, 1001u);
    EXPECT_EQ(table.retired_pending(), 0u);
}

TEST(ExportSet, ApplyRejectsBadPlansWithoutPublishing) {
    core::ExportTable table;
    ASSERT_TRUE(table.add(export_cfg(1, "/export/one"), mem(1)).has_value());
    auto before = table.snapshot();
    auto expect_rejected = [&](core::ExportSetPlan plan, size_t started_count) {
        auto started = std::vector<std::unique_ptr<backend::Backend>>();
        for (size_t i = 0; i < started_count; ++i) started.push_back(mem(9));
        auto applied = table.apply(std::move(plan), started, 1);
        EXPECT_FALSE(applied.has_value());
        if (!applied) EXPECT_EQ(static_cast<int>(applied.error()), EINVAL);
        // handed back untouched
        EXPECT_EQ(started.size(), started_count);
        EXPECT_TRUE(table.snapshot() == before);
        EXPECT_EQ(table.retired_pending(), 0u);
    };
    core::ExportSetPlan p;
    p.add.push_back(export_cfg(2, "/export/two"));
    // backend count mismatch
    expect_rejected(p, 0);
    expect_rejected(p, 2);
    p = {};
    p.add.push_back(export_cfg(1, "/export/dup"));
    // fsid already in the set
    expect_rejected(p, 1);
    p = {};
    p.add.push_back(export_cfg(0, "/export/zero"));
    expect_rejected(p, 1);
    p = {};
    p.add.push_back(export_cfg(2, "/export/two"));
    p.add.push_back(export_cfg(2, "/export/two-again"));
    // listed twice
    expect_rejected(p, 2);
    p = {};
    p.update.push_back(export_cfg(7, "/export/seven"));
    // unknown update
    expect_rejected(p, 0);
    p = {};
    p.update.push_back(export_cfg(1, "/export/moved"));
    // path change is not an update
    expect_rejected(p, 0);
    p = {};
    p.remove.push_back(7);
    // unknown remove
    expect_rejected(p, 0);
    p = {};
    p.remove.push_back(1);
    p.update.push_back(export_cfg(1, "/export/one"));
    // update and remove of one fsid
    expect_rejected(p, 0);
    p = {};
    auto bad = export_cfg(2, "/export/two");
    bad.clients = {"not-a-cidr"};
    p.add.push_back(bad);
    expect_rejected(p, 1);
    p = {};
    bad = export_cfg(1, "/export/one");
    bad.clients = {"300.0.0.0/8"};
    p.update.push_back(bad);
    expect_rejected(p, 0);
    // The entry was never touched by the rejected update.
    EXPECT_EQ(before->by_fsid(1)->client_list().size(), 1u);

    // An empty plan is a valid (if pointless) publish.
    std::vector<std::unique_ptr<backend::Backend>> none;
    auto empty = table.apply({}, none, 1);
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ((*empty)->generation, before->generation + 1);
    EXPECT_TRUE((*empty)->by_fsid(1) == before->by_fsid(1));
}
