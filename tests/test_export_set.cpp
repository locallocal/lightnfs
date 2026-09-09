// Export-set snapshots (plan 12 B1): the export table publishes immutable ExportSet
// versions; a reader's snapshot is stable while later publishes swap the table's
// current set, entries are shared across versions, and the pseudo tree travels with
// the set.

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
  EXPECT_EQ(first->generation, 3u);  // one publish per add on top of the empty set
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
  hit = set->for_mount_path("/export/deeper", rel);  // not a component boundary
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
  EXPECT_EQ(boot->pseudo->attr_of(*boot->pseudo->root()).change, 1u);

  table.set_epoch(42);
  auto served = table.snapshot();
  EXPECT_TRUE(served != boot);
  EXPECT_EQ(served->epoch, 42u);
  EXPECT_EQ(served->generation, boot->generation + 1);
  EXPECT_EQ(served->pseudo->attr_of(*served->pseudo->root()).change, 42u);
  EXPECT_EQ(served->pseudo->attr_of(*served->pseudo->for_export(1)).change, 42u);
  // Same entries, same pseudo ids; the earlier snapshot keeps its own change base.
  EXPECT_TRUE(served->by_fsid(1) == boot->by_fsid(1));
  EXPECT_EQ(served->pseudo->for_export(1)->id, boot->pseudo->for_export(1)->id);
  EXPECT_EQ(boot->pseudo->attr_of(*boot->pseudo->root()).change, 1u);
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
