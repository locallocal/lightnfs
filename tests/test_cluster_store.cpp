// Shared cluster state (design 09 §9.4, plan 10 A2): the atomic file helper and the
// POSIX ClusterStore over a temporary directory — key creation/reuse, monotonic epoch
// across two store objects, fence acquire/renew/release with expiry, force and stale
// lock recovery, the reclaim list round trip and the per-node export digests.

#include "mini_test.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "core/atomic_file.hpp"
#include "core/boot_epoch.hpp"
#include "core/config.hpp"
#include "mem_cluster_store.hpp"
#include "server/cluster_store.hpp"
#include "util/sha256.hpp"

using namespace lnfs;
using namespace std::chrono_literals;

namespace {

struct TmpDir {
  std::string path;
  TmpDir() {
    char tmpl[] = "/tmp/lnfs-cluster-XXXXXX";
    path = mkdtemp(tmpl);
  }
  ~TmpDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

std::string slurp(const std::string& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

void write_raw(const std::string& path, const std::string& text) {
  std::ofstream out(path);
  out << text;
}

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool has_tmp_leftovers(const std::string& dir) {
  for (const auto& entry : std::filesystem::directory_iterator(dir))
    if (entry.path().filename().string().find(".tmp.") != std::string::npos) return true;
  return false;
}

}  // namespace

TEST(ClusterStore, AtomicWriteFileReplacesAndReadsBack) {
  TmpDir dir;
  std::string path = dir.path + "/data";
  auto missing = core::read_file_if_exists(path);
  ASSERT_TRUE(missing.has_value());
  EXPECT_FALSE(missing->has_value());

  ASSERT_TRUE(core::atomic_write_file(path, "first\n").has_value());
  EXPECT_STREQ(slurp(path), "first\n");
  struct stat st {};
  ASSERT_TRUE(::stat(path.c_str(), &st) == 0);
  EXPECT_EQ(st.st_mode & 0777, 0600u);

  ASSERT_TRUE(core::atomic_write_file(path, "second\n", 0644).has_value());
  EXPECT_STREQ(slurp(path), "second\n");
  ASSERT_TRUE(::stat(path.c_str(), &st) == 0);
  EXPECT_EQ(st.st_mode & 0777, 0644u);  // mode applies to the fresh temp file
  EXPECT_FALSE(has_tmp_leftovers(dir.path));

  auto back = core::read_file_if_exists(path);
  ASSERT_TRUE(back.has_value() && back->has_value());
  EXPECT_STREQ(**back, "second\n");

  // A missing parent directory fails without touching anything.
  EXPECT_FALSE(core::atomic_write_file(dir.path + "/nope/data", "x").has_value());

  // The boot epoch still persists through the shared helper.
  auto e1 = core::bump_boot_epoch(dir.path);
  auto e2 = core::bump_boot_epoch(dir.path);
  ASSERT_TRUE(e1.has_value() && e2.has_value());
  EXPECT_EQ(*e1, 1u);
  EXPECT_EQ(*e2, 2u);
  EXPECT_STREQ(slurp(dir.path + "/boot_epoch"), "2\n");
}

TEST(ClusterStore, KeyCreatedOnceThenShared) {
  TmpDir dir;
  auto a = server::make_posix_cluster_store(dir.path + "/shared/");  // trailing slash ok
  auto b = server::make_posix_cluster_store(dir.path + "/shared");
  auto ka = a->load_or_create_key();
  ASSERT_TRUE(ka.has_value());
  struct stat st {};
  ASSERT_TRUE(::stat((dir.path + "/shared/hmac.key").c_str(), &st) == 0);
  EXPECT_EQ(st.st_mode & 0777, 0600u);
  EXPECT_EQ(st.st_size, 16);
  auto kb = b->load_or_create_key();
  ASSERT_TRUE(kb.has_value());
  EXPECT_TRUE(*ka == *kb);
  bool nonzero = std::any_of(ka->begin(), ka->end(), [](std::byte x) { return x != std::byte{0}; });
  EXPECT_TRUE(nonzero);
}

TEST(ClusterStore, EpochMonotonicAcrossStores) {
  TmpDir dir;
  auto a = server::make_posix_cluster_store(dir.path);
  auto b = server::make_posix_cluster_store(dir.path);
  auto none = a->read_epoch();
  ASSERT_TRUE(none.has_value());
  EXPECT_EQ(*none, 0u);
  uint64_t expect = 0;
  for (int i = 0; i < 3; ++i) {
    auto ea = a->bump_epoch();
    ASSERT_TRUE(ea.has_value());
    EXPECT_EQ(*ea, ++expect);
    auto eb = b->bump_epoch();
    ASSERT_TRUE(eb.has_value());
    EXPECT_EQ(*eb, ++expect);
  }
  auto ra = a->read_epoch();
  auto rb = b->read_epoch();
  ASSERT_TRUE(ra.has_value() && rb.has_value());
  EXPECT_EQ(*ra, 6u);
  EXPECT_EQ(*rb, 6u);
  EXPECT_FALSE(std::filesystem::exists(dir.path + "/epoch.lock"));  // released
  EXPECT_FALSE(has_tmp_leftovers(dir.path));
}

TEST(ClusterStore, FenceAcquireRenewReleaseAndExpiry) {
  TmpDir dir;
  auto a = server::make_posix_cluster_store(dir.path);
  auto b = server::make_posix_cluster_store(dir.path);

  auto empty = a->read_fence();
  ASSERT_TRUE(empty.has_value());
  EXPECT_FALSE(empty->has_value());

  int64_t before = now_ms();
  auto held = a->acquire_fence("gw a", 7, 1000ms, false);  // node names may hold spaces
  ASSERT_TRUE(held.has_value());
  EXPECT_STREQ(held->node, "gw a");
  EXPECT_EQ(held->epoch, 7u);
  EXPECT_TRUE(held->expires_at_ms >= before + 1000);

  // Someone else: busy while the lease is live; the record is untouched.
  auto busy = b->acquire_fence("gw-b", 8, 1000ms, false);
  ASSERT_TRUE(!busy.has_value());
  EXPECT_EQ(static_cast<int>(busy.error()), EBUSY);
  auto seen = b->read_fence();
  ASSERT_TRUE(seen.has_value() && seen->has_value());
  EXPECT_STREQ((*seen)->node, "gw a");
  EXPECT_EQ((*seen)->epoch, 7u);

  // The holder itself may re-acquire (a restarted process on the same node).
  auto again = a->acquire_fence("gw a", 7, 1000ms, false);
  ASSERT_TRUE(again.has_value());

  // renew: ours extends, someone else's is EPERM.
  auto renewed = a->renew_fence("gw a", 5000ms);
  EXPECT_TRUE(renewed.has_value());
  auto after = a->read_fence();
  ASSERT_TRUE(after.has_value() && after->has_value());
  EXPECT_TRUE((*after)->expires_at_ms >= now_ms() + 4000);
  auto not_ours = b->renew_fence("gw-b", 1000ms);
  ASSERT_TRUE(!not_ours.has_value());
  EXPECT_EQ(static_cast<int>(not_ours.error()), EPERM);

  // Expired lease (beyond the skew tolerance): the other node takes over.
  write_raw(dir.path + "/fence", "7 " + std::to_string(now_ms() - 1000) + " gw a\n");
  auto taken = b->acquire_fence("gw-b", 8, 1000ms, false);
  ASSERT_TRUE(taken.has_value());
  EXPECT_STREQ(taken->node, "gw-b");
  EXPECT_EQ(taken->epoch, 8u);
  // Within the tolerance the lease still counts as live.
  write_raw(dir.path + "/fence", "8 " + std::to_string(now_ms() - 100) + " gw-b\n");
  auto still = a->acquire_fence("gw a", 9, 1000ms, false);
  ASSERT_TRUE(!still.has_value());
  EXPECT_EQ(static_cast<int>(still.error()), EBUSY);

  // The old holder finds out on its next renew, and cannot release the new record.
  auto lost = a->renew_fence("gw a", 1000ms);
  ASSERT_TRUE(!lost.has_value());
  EXPECT_EQ(static_cast<int>(lost.error()), EPERM);
  auto cannot_release = a->release_fence("gw a");
  ASSERT_TRUE(!cannot_release.has_value());
  EXPECT_EQ(static_cast<int>(cannot_release.error()), EPERM);

  // force: the operator's manual takeover ignores a live lease.
  auto forced = a->acquire_fence("gw a", 9, 1000ms, true);
  ASSERT_TRUE(forced.has_value());
  EXPECT_EQ(forced->epoch, 9u);

  // release: ours removes the record, releasing nothing is fine.
  EXPECT_TRUE(a->release_fence("gw a").has_value());
  auto gone = b->read_fence();
  ASSERT_TRUE(gone.has_value());
  EXPECT_FALSE(gone->has_value());
  EXPECT_TRUE(b->release_fence("gw-b").has_value());
  EXPECT_FALSE(std::filesystem::exists(dir.path + "/fence.lock"));

  // A corrupt record is an error, not a silent "no fence".
  write_raw(dir.path + "/fence", "garbage\n");
  EXPECT_FALSE(a->read_fence().has_value());
}

TEST(ClusterStore, StaleLockFileIsReclaimedLiveLockIsBusy) {
  TmpDir dir;
  auto store = server::make_posix_cluster_store(dir.path, 600ms);
  std::filesystem::create_directories(dir.path + "/clients");

  // A lock left by a dead writer (stamped well in the past) is reclaimed.
  write_raw(dir.path + "/epoch.lock", "12345 " + std::to_string(now_ms() - 10000) + "\n");
  auto bumped = store->bump_epoch();
  ASSERT_TRUE(bumped.has_value());
  EXPECT_EQ(*bumped, 1u);
  EXPECT_FALSE(std::filesystem::exists(dir.path + "/epoch.lock"));

  // An unreadable lock falls back to its mtime: fresh → busy (after the bounded wait,
  // well inside the staleness window), old → reclaimed.
  write_raw(dir.path + "/fence.lock", "");
  auto busy = store->acquire_fence("gw", 1, 1000ms, false);
  ASSERT_TRUE(!busy.has_value());
  EXPECT_EQ(static_cast<int>(busy.error()), EBUSY);
  EXPECT_TRUE(std::filesystem::exists(dir.path + "/fence.lock"));  // not ours to remove
  std::this_thread::sleep_for(700ms);
  auto reclaimed = store->acquire_fence("gw", 1, 1000ms, false);
  ASSERT_TRUE(reclaimed.has_value());
  EXPECT_FALSE(std::filesystem::exists(dir.path + "/fence.lock"));

  // A live lock (stamped now, and mtime now) stays busy for the bounded retry window.
  write_raw(dir.path + "/fence.lock", "999 " + std::to_string(now_ms()) + "\n");
  auto live = store->renew_fence("gw", 1000ms);
  ASSERT_TRUE(!live.has_value());
  EXPECT_EQ(static_cast<int>(live.error()), EBUSY);
}

TEST(ClusterStore, ClientListRoundTrip) {
  TmpDir dir;
  auto store = server::make_posix_cluster_store(dir.path);
  auto none = store->list_clients();
  ASSERT_TRUE(none.has_value());
  EXPECT_EQ(none->size(), 0u);

  std::string binary_owner("linux\0nfs\x01\xff", 10);
  ASSERT_TRUE(store->put_client("owner-a").has_value());
  ASSERT_TRUE(store->put_client("owner-b").has_value());
  ASSERT_TRUE(store->put_client(binary_owner).has_value());
  ASSERT_TRUE(store->put_client("owner-a").has_value());  // idempotent
  auto listed = store->list_clients();
  ASSERT_TRUE(listed.has_value());
  std::sort(listed->begin(), listed->end());
  ASSERT_TRUE(listed->size() == 3u);
  std::vector<std::string> expect{"owner-a", "owner-b", binary_owner};
  std::sort(expect.begin(), expect.end());
  EXPECT_TRUE(*listed == expect);

  // File naming matches state_dir/clients/ (fnv64 of the owner, 16 hex digits).
  int named = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir.path + "/clients")) {
    auto name = entry.path().filename().string();
    EXPECT_EQ(name.size(), 16u);
    ++named;
  }
  EXPECT_EQ(named, 3);

  ASSERT_TRUE(store->erase_client("owner-b").has_value());
  ASSERT_TRUE(store->erase_client("owner-b").has_value());  // missing is fine
  auto left = store->list_clients();
  ASSERT_TRUE(left.has_value());
  EXPECT_EQ(left->size(), 2u);
  EXPECT_TRUE(std::find(left->begin(), left->end(), "owner-b") == left->end());

  // Another store object over the same directory (the taking-over gateway) sees it.
  auto peer = server::make_posix_cluster_store(dir.path);
  auto peer_list = peer->list_clients();
  ASSERT_TRUE(peer_list.has_value());
  EXPECT_EQ(peer_list->size(), 2u);
}

TEST(ClusterStore, ExportDigestsPerNode) {
  TmpDir dir;
  auto a = server::make_posix_cluster_store(dir.path);
  auto b = server::make_posix_cluster_store(dir.path);
  auto none = a->list_exports_digests();
  ASSERT_TRUE(none.has_value());
  EXPECT_EQ(none->size(), 0u);

  ASSERT_TRUE(a->put_exports_digest("gw1", "sha256:aaaa").has_value());
  ASSERT_TRUE(b->put_exports_digest("gw2", "sha256:bbbb").has_value());
  ASSERT_TRUE(a->put_exports_digest("gw1", "sha256:cccc").has_value());  // overwrite
  auto listed = b->list_exports_digests();
  ASSERT_TRUE(listed.has_value());
  std::sort(listed->begin(), listed->end());
  ASSERT_TRUE(listed->size() == 2u);
  EXPECT_STREQ((*listed)[0].first, "gw1");
  EXPECT_STREQ((*listed)[0].second, "sha256:cccc");
  EXPECT_STREQ((*listed)[1].first, "gw2");
  EXPECT_STREQ((*listed)[1].second, "sha256:bbbb");
  // The other files in the directory are not mistaken for digests.
  ASSERT_TRUE(a->bump_epoch().has_value());
  ASSERT_TRUE(a->put_client("x").has_value());
  auto still = a->list_exports_digests();
  ASSERT_TRUE(still.has_value());
  EXPECT_EQ(still->size(), 2u);
}

// Two gateways starting on a fresh shared directory at the same moment (plan 10 B1
// found this): every creator must end up with the same 16 bytes, no reader may ever
// see a partial key, and nothing is left behind.
TEST(ClusterStore, KeyConcurrentCreatorsAgree) {
  for (int round = 0; round < 20; ++round) {
    TmpDir dir;
    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    std::vector<Result<std::array<std::byte, 16>>> keys(kThreads, Err(errno_from(EIO)));
    for (int i = 0; i < kThreads; ++i)
      threads.emplace_back([&, i] {
        auto store = server::make_posix_cluster_store(dir.path);
        keys[static_cast<size_t>(i)] = store->load_or_create_key();
      });
    for (auto& t : threads) t.join();
    for (int i = 0; i < kThreads; ++i) {
      ASSERT_TRUE(keys[static_cast<size_t>(i)].has_value());
      EXPECT_TRUE(*keys[static_cast<size_t>(i)] == *keys[0]);
    }
    EXPECT_FALSE(has_tmp_leftovers(dir.path));
    struct stat st {};
    ASSERT_TRUE(::stat((dir.path + "/hmac.key").c_str(), &st) == 0);
    EXPECT_EQ(st.st_size, 16);
    EXPECT_EQ(st.st_mode & 0777, 0600u);
  }
}

// FIPS 180-4 vectors for the digest's hash.
TEST(ClusterStore, Sha256Vectors) {
  EXPECT_STREQ(util::sha256_hex(std::string_view("")),
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_STREQ(util::sha256_hex(std::string_view("abc")),
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_STREQ(util::sha256_hex(std::string_view(
                   "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // Padding boundaries: 55 / 56 / 63 / 64 / 65 bytes cross the one-block limit.
  EXPECT_STREQ(util::sha256_hex(std::string(55, 'a')),
               "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  EXPECT_STREQ(util::sha256_hex(std::string(56, 'a')),
               "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  EXPECT_STREQ(util::sha256_hex(std::string(64, 'a')),
               "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
  EXPECT_STREQ(util::sha256_hex(std::string(1000000, 'a')),
               "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// Export-table digest (design 09 §9.3, plan 10 B4): per-node keys (credentials, log
// paths, cache sizes) do not change it; the tree identity (subdir, fsid, path,
// backend, squash) does.  Export order in the file does not matter.
TEST(ClusterStore, ExportDigestIgnoresPerNodeKeys) {
  auto parse = [](const std::string& text) {
    auto cfg = core::parse_config(text);
    if (!cfg.has_value()) MT_FAIL("parse failed");
    return cfg.has_value() ? std::move(*cfg) : core::Config{};
  };
  const std::string a =
      "[[export]]\npath = \"/vol\"\nbackend = \"cephfs\"\nfsid = 1\nsquash = \"root\"\n"
      "clients = [\"10.0.0.0/8\"]\n"
      "[export.cephfs]\nfs_name = \"cephfs\"\nsubdir = \"/exports/a\"\n"
      "keyring = \"/etc/ceph/gw1.keyring\"\nid = \"gw1\"\nlog_file = \"/var/log/gw1.log\"\n"
      "fd_cache = 1024\nmon_host = \"mon1\"\nconf = \"/etc/ceph/gw1.conf\"\n"
      "[[export]]\npath = \"/scratch\"\nbackend = \"gluster\"\nfsid = 2\n"
      "[export.gluster]\nvolume = \"scratch\"\n";
  const std::string b =  // other node: different credentials/logs/cache, exports swapped
      "[[export]]\npath = \"/scratch\"\nbackend = \"gluster\"\nfsid = 2\n"
      "[export.gluster]\nvolume = \"scratch\"\n"
      "[[export]]\npath = \"/vol\"\nbackend = \"cephfs\"\nfsid = 1\n"
      "clients = [\"192.168.0.0/16\"]\nread_bps = \"10MiB\"\n"
      "[export.cephfs]\nsubdir = \"/exports/a\"\nfs_name = \"cephfs\"\n"
      "keyring = \"/etc/ceph/gw2.keyring\"\nid = \"gw2\"\nlog_file = \"/var/log/gw2.log\"\n"
      "fd_cache = 4096\nmon_host = \"mon2\"\nconf = \"/etc/ceph/gw2.conf\"\n";
  auto da = core::canonical_exports_digest(parse(a));
  auto db = core::canonical_exports_digest(parse(b));
  EXPECT_TRUE(da.starts_with("sha256:"));
  EXPECT_EQ(da.size(), 7u + 64u);
  EXPECT_STREQ(da, db);
  auto text = core::canonical_exports_text(parse(a));
  EXPECT_TRUE(text.find("keyring") == std::string::npos);
  EXPECT_TRUE(text.find("subdir=/exports/a") != std::string::npos);
  EXPECT_TRUE(text.find("fsid=1") < text.find("fsid=2"));  // sorted by fsid

  // Each identity-bearing difference changes the digest.
  auto differs = [&](const std::string& from, const std::string& to) {
    std::string mutated = a;
    size_t at = mutated.find(from);
    if (at == std::string::npos) MT_FAIL("pattern missing");
    mutated.replace(at, from.size(), to);
    return core::canonical_exports_digest(parse(mutated)) != da;
  };
  EXPECT_TRUE(differs("subdir = \"/exports/a\"", "subdir = \"/exports/b\""));
  EXPECT_TRUE(differs("fsid = 1\n", "fsid = 3\n"));
  EXPECT_TRUE(differs("path = \"/vol\"", "path = \"/volume\""));
  EXPECT_TRUE(differs("squash = \"root\"", "squash = \"none\""));
  EXPECT_TRUE(differs("fs_name = \"cephfs\"", "fs_name = \"other\""));
  EXPECT_TRUE(differs("volume = \"scratch\"", "volume = \"scratch2\""));
  EXPECT_TRUE(differs("[[export]]\npath = \"/scratch\"",
                      "[[export]]\nreadonly = true\npath = \"/scratch\""));
  // Per-node keys and client policy do not.
  EXPECT_FALSE(differs("fd_cache = 1024", "fd_cache = 8"));
  EXPECT_FALSE(differs("clients = [\"10.0.0.0/8\"]", "clients = [\"127.0.0.0/8\"]"));
}

// ---- active-active (design 10 §10.3, plan 12 A2) -------------------------------------

TEST(ClusterStore, FsFenceBatchedPerNode) {
  TmpDir dir;
  auto a = server::make_posix_cluster_store(dir.path);
  auto b = server::make_posix_cluster_store(dir.path);

  auto none = a->read_fs_fence(1);
  ASSERT_TRUE(none.has_value());
  EXPECT_FALSE(none->has_value());
  auto no_records = a->list_fences();
  ASSERT_TRUE(no_records.has_value());
  EXPECT_EQ(no_records->size(), 0u);

  // A takes F1 and F2: one record file, both exports under one lease.
  int64_t before = now_ms();
  auto f1 = a->acquire_fs_fence(1, "gw1", 5, 1000ms, false);
  ASSERT_TRUE(f1.has_value());
  EXPECT_STREQ(f1->node, "gw1");
  EXPECT_EQ(f1->epoch, 5u);
  EXPECT_TRUE(f1->expires_at_ms >= before + 1000);
  ASSERT_TRUE(a->acquire_fs_fence(2, "gw1", 3, 1000ms, false).has_value());
  EXPECT_TRUE(std::filesystem::exists(dir.path + "/fence.gw1"));
  EXPECT_FALSE(std::filesystem::exists(dir.path + "/fence"));  // the failover file: untouched
  auto records = a->list_fences();
  ASSERT_TRUE(records.has_value());
  ASSERT_TRUE(records->size() == 1u);
  EXPECT_STREQ((*records)[0].node, "gw1");
  ASSERT_TRUE((*records)[0].holds.size() == 2u);
  EXPECT_EQ((*records)[0].holds[0].fsid, 1u);
  EXPECT_EQ((*records)[0].holds[0].epoch, 5u);
  EXPECT_EQ((*records)[0].holds[1].fsid, 2u);
  EXPECT_EQ((*records)[0].holds[1].epoch, 3u);

  // B: F1 is busy, F3 is free; B's record lists only F3.
  auto busy = b->acquire_fs_fence(1, "gw2", 6, 1000ms, false);
  ASSERT_TRUE(!busy.has_value());
  EXPECT_EQ(static_cast<int>(busy.error()), EBUSY);
  ASSERT_TRUE(b->acquire_fs_fence(3, "gw2", 1, 1000ms, false).has_value());
  auto f1_seen = b->read_fs_fence(1);
  ASSERT_TRUE(f1_seen.has_value() && f1_seen->has_value());
  EXPECT_STREQ((*f1_seen)->node, "gw1");
  EXPECT_EQ((*f1_seen)->epoch, 5u);
  auto f3_seen = a->read_fs_fence(3);
  ASSERT_TRUE(f3_seen.has_value() && f3_seen->has_value());
  EXPECT_STREQ((*f3_seen)->node, "gw2");
  // The holder itself may re-acquire (a restarted process): the epoch is refreshed.
  auto again = a->acquire_fs_fence(1, "gw1", 7, 1000ms, false);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(again->epoch, 7u);

  // renew: one write covers every export A holds; the list is preserved.
  ASSERT_TRUE(a->renew_fences("gw1", 5000ms).has_value());
  auto renewed = b->read_fs_fence(2);
  ASSERT_TRUE(renewed.has_value() && renewed->has_value());
  EXPECT_TRUE((*renewed)->expires_at_ms >= now_ms() + 4000);
  std::string raw = slurp(dir.path + "/fence.gw1");
  EXPECT_TRUE(raw.find(" 1:7,2:3\n") != std::string::npos);

  // A's lease lapses: F1 goes to B, and A's record no longer names it (so a late
  // heartbeat from A cannot revive the hold); F2 stays listed until someone takes it.
  write_raw(dir.path + "/fence.gw1", std::to_string(now_ms() - 1000) + " 1:7,2:3\n");
  auto lapsed = b->read_fs_fence(1);  // the expired record is still reported
  ASSERT_TRUE(lapsed.has_value() && lapsed->has_value());
  EXPECT_STREQ((*lapsed)->node, "gw1");
  auto taken = b->acquire_fs_fence(1, "gw2", 8, 1000ms, false);
  ASSERT_TRUE(taken.has_value());
  EXPECT_STREQ(taken->node, "gw2");
  ASSERT_TRUE(a->renew_fences("gw1", 1000ms).has_value());  // A comes back: F2 only
  auto f1_now = a->read_fs_fence(1);
  ASSERT_TRUE(f1_now.has_value() && f1_now->has_value());
  EXPECT_STREQ((*f1_now)->node, "gw2");
  EXPECT_EQ((*f1_now)->epoch, 8u);
  auto f2_now = b->read_fs_fence(2);
  ASSERT_TRUE(f2_now.has_value() && f2_now->has_value());
  EXPECT_STREQ((*f2_now)->node, "gw1");
  // Within the skew tolerance a record still counts as live.
  write_raw(dir.path + "/fence.gw2", std::to_string(now_ms() - 100) + " 1:8,3:1\n");
  auto still = a->acquire_fs_fence(1, "gw1", 9, 1000ms, false);
  ASSERT_TRUE(!still.has_value());
  EXPECT_EQ(static_cast<int>(still.error()), EBUSY);

  // release: not ours → EPERM; ours → dropped, the record stays as a heartbeat;
  // nobody's → ok.
  auto not_ours = a->release_fs_fence(1, "gw1");
  ASSERT_TRUE(!not_ours.has_value());
  EXPECT_EQ(static_cast<int>(not_ours.error()), EPERM);
  ASSERT_TRUE(a->release_fs_fence(2, "gw1").has_value());
  EXPECT_TRUE(std::filesystem::exists(dir.path + "/fence.gw1"));
  auto heartbeat = a->list_fences();
  ASSERT_TRUE(heartbeat.has_value() && heartbeat->size() == 2u);
  EXPECT_STREQ((*heartbeat)[0].node, "gw1");
  EXPECT_EQ((*heartbeat)[0].holds.size(), 0u);
  auto f2_gone = b->read_fs_fence(2);
  ASSERT_TRUE(f2_gone.has_value());
  EXPECT_FALSE(f2_gone->has_value());
  EXPECT_TRUE(a->release_fs_fence(2, "gw1").has_value());
  ASSERT_TRUE(a->renew_fences("gw1", 1000ms).has_value());  // an empty record still renews
  EXPECT_TRUE(slurp(dir.path + "/fence.gw1").find(' ') == std::string::npos);

  // force: the operator's manual takeover ignores a live lease, and the previous
  // holder's record is stripped of the export.
  auto forced = a->acquire_fs_fence(3, "gw1", 2, 1000ms, true);
  ASSERT_TRUE(forced.has_value());
  auto stripped = a->list_fences();
  ASSERT_TRUE(stripped.has_value() && stripped->size() == 2u);
  ASSERT_TRUE((*stripped)[1].holds.size() == 1u);  // gw2: only F1 left
  EXPECT_EQ((*stripped)[1].holds[0].fsid, 1u);
  EXPECT_FALSE(std::filesystem::exists(dir.path + "/fence.lock"));
  EXPECT_FALSE(has_tmp_leftovers(dir.path));

  // A node that never held anything can still heart-beat, and a corrupt record is an
  // error rather than a silent "free".
  ASSERT_TRUE(b->renew_fences("gw3", 1000ms).has_value());
  auto three = a->list_fences();
  ASSERT_TRUE(three.has_value());
  EXPECT_EQ(three->size(), 3u);
  write_raw(dir.path + "/fence.gw3", "garbage\n");
  EXPECT_FALSE(a->list_fences().has_value());
  EXPECT_FALSE(a->read_fs_fence(1).has_value());
}

TEST(ClusterStore, PerFsidEpochOwnerClientsAndNodes) {
  TmpDir dir;
  auto a = server::make_posix_cluster_store(dir.path);
  auto b = server::make_posix_cluster_store(dir.path);

  // Node epochs: independent per node, monotonic across store objects.
  auto e0 = a->read_node_epoch("gw1");
  ASSERT_TRUE(e0.has_value());
  EXPECT_EQ(*e0, 0u);
  ASSERT_TRUE(a->bump_node_epoch("gw1").has_value());
  auto e2 = b->bump_node_epoch("gw1");
  ASSERT_TRUE(e2.has_value());
  EXPECT_EQ(*e2, 2u);
  auto other = a->bump_node_epoch("gw2");
  ASSERT_TRUE(other.has_value());
  EXPECT_EQ(*other, 1u);
  EXPECT_EQ(*a->read_node_epoch("gw1"), 2u);
  EXPECT_EQ(*a->read_epoch(), 0u);  // the failover epoch is a different counter
  EXPECT_FALSE(std::filesystem::exists(dir.path + "/epoch.gw1.lock"));
  // The per-node epoch files — and the catalog's files (plan 12 A3) — are not mistaken
  // for export digests or fence records.
  ASSERT_TRUE(a->put_exports_digest("gw1", "sha256:aaaa").has_value());
  ASSERT_TRUE(a->write_catalog(0, "[catalog]\nversion = 1\n").has_value());
  ASSERT_TRUE(a->put_catalog_applied({"gw1", 1, "sha256:aaaa", now_ms(), "ok"}).has_value());
  auto digests = a->list_exports_digests();
  ASSERT_TRUE(digests.has_value());
  EXPECT_EQ(digests->size(), 1u);
  auto fences = a->list_fences();
  ASSERT_TRUE(fences.has_value());
  EXPECT_EQ(fences->size(), 0u);

  // Node addresses.
  auto no_nodes = a->list_nodes();
  ASSERT_TRUE(no_nodes.has_value());
  EXPECT_EQ(no_nodes->size(), 0u);
  ASSERT_TRUE(a->put_node_address("gw2", "10.0.0.12:2049").has_value());
  ASSERT_TRUE(b->put_node_address("gw1", "10.0.0.11:2049").has_value());
  ASSERT_TRUE(a->put_node_address("gw1", "[fd00::11]:2049").has_value());  // overwrite
  auto nodes = b->list_nodes();
  ASSERT_TRUE(nodes.has_value() && nodes->size() == 2u);
  EXPECT_STREQ((*nodes)[0].first, "gw1");
  EXPECT_STREQ((*nodes)[0].second, "[fd00::11]:2049");
  EXPECT_STREQ((*nodes)[1].first, "gw2");
  EXPECT_STREQ((*nodes)[1].second, "10.0.0.12:2049");

  // Per-fsid epochs.
  EXPECT_EQ(*a->read_fs_epoch(7), 0u);
  ASSERT_TRUE(a->bump_fs_epoch(7).has_value());
  auto fe = b->bump_fs_epoch(7);
  ASSERT_TRUE(fe.has_value());
  EXPECT_EQ(*fe, 2u);
  EXPECT_EQ(*a->bump_fs_epoch(8), 1u);
  EXPECT_EQ(*b->read_fs_epoch(7), 2u);
  EXPECT_TRUE(std::filesystem::exists(dir.path + "/fs/7/epoch"));
  EXPECT_FALSE(std::filesystem::exists(dir.path + "/fs/7/epoch.lock"));

  // Owner records: absent, then round-tripped; corrupt is an error.
  auto no_owner = a->read_owner(7);
  ASSERT_TRUE(no_owner.has_value());
  EXPECT_FALSE(no_owner->has_value());
  ASSERT_TRUE(a->put_owner(7, {"gw1", "[fd00::11]:2049", 2}).has_value());
  auto owner = b->read_owner(7);
  ASSERT_TRUE(owner.has_value() && owner->has_value());
  EXPECT_STREQ((*owner)->node, "gw1");
  EXPECT_STREQ((*owner)->address, "[fd00::11]:2049");
  EXPECT_EQ((*owner)->fs_epoch, 2u);
  EXPECT_FALSE(a->put_owner(7, {"", "x:1", 1}).has_value());
  EXPECT_FALSE(a->put_owner(7, {"gw1", "bad address", 1}).has_value());
  write_raw(dir.path + "/fs/7/owner", "2 only-two\n");
  EXPECT_FALSE(a->read_owner(7).has_value());

  // Per-fsid reclaim lists: separate per export and from the global list.
  ASSERT_TRUE(a->put_client(7, "Linux NFSv4.1 host-a/1").has_value());
  ASSERT_TRUE(a->put_client(7, "owner-b").has_value());
  ASSERT_TRUE(a->put_client(8, "owner-c").has_value());
  ASSERT_TRUE(a->put_client("global-owner").has_value());
  auto seven = b->list_clients(7);
  ASSERT_TRUE(seven.has_value());
  std::sort(seven->begin(), seven->end());
  ASSERT_TRUE(seven->size() == 2u);
  EXPECT_STREQ((*seven)[0], "Linux NFSv4.1 host-a/1");
  EXPECT_STREQ((*seven)[1], "owner-b");
  auto eight = b->list_clients(8);
  ASSERT_TRUE(eight.has_value() && eight->size() == 1u);
  auto global = b->list_clients();
  ASSERT_TRUE(global.has_value() && global->size() == 1u);
  EXPECT_STREQ((*global)[0], "global-owner");
  auto nine = b->list_clients(9);  // never touched: empty, not an error
  ASSERT_TRUE(nine.has_value());
  EXPECT_EQ(nine->size(), 0u);
  ASSERT_TRUE(a->erase_client(7, "owner-b").has_value());
  ASSERT_TRUE(a->erase_client(7, "never-there").has_value());
  auto left = b->list_clients(7);
  ASSERT_TRUE(left.has_value() && left->size() == 1u);
  EXPECT_STREQ((*left)[0], "Linux NFSv4.1 host-a/1");
  EXPECT_FALSE(has_tmp_leftovers(dir.path + "/fs/7/clients"));
}

// ---- shared export catalog (design 11 §11.3, plan 12 A3) ------------------------------

namespace {

std::string catalog_text(uint64_t version, const std::string& tag = "") {
  return "[catalog]\nversion = " + std::to_string(version) + "\ncomment = \"" + tag +
         "\"\n[[export]]\npath = \"/export/a\"\nfsid = 1\nbackend = \"local\"\n";
}

}  // namespace

TEST(ClusterStore, CatalogCasAndHistory) {
  TmpDir dir;
  auto a = server::make_posix_cluster_store(dir.path);
  auto b = server::make_posix_cluster_store(dir.path);

  // Nothing published yet: nullopt, no history, a stale expectation is EAGAIN.
  auto none = a->read_catalog();
  ASSERT_TRUE(none.has_value());
  EXPECT_FALSE(none->has_value());
  auto no_history = a->list_catalog_history();
  ASSERT_TRUE(no_history.has_value() && no_history->empty());
  auto early = a->write_catalog(1, catalog_text(2));
  ASSERT_TRUE(!early.has_value());
  EXPECT_TRUE(early.error() == errno_from(EAGAIN));
  // The text must say expected + 1 and must have a header at all.
  auto wrong = a->write_catalog(0, catalog_text(5));
  ASSERT_TRUE(!wrong.has_value());
  EXPECT_TRUE(wrong.error() == errno_from(EINVAL));
  auto headless = a->write_catalog(0, "[[export]]\npath = \"/a\"\nfsid = 1\n");
  ASSERT_TRUE(!headless.has_value());
  EXPECT_TRUE(headless.error() == errno_from(EINVAL));
  EXPECT_FALSE(std::filesystem::exists(dir.path + "/catalog.toml"));

  // First commit: expected 0 → version 1, read back verbatim, lock released.
  auto v1 = a->write_catalog(0, catalog_text(1, "first"));
  ASSERT_TRUE(v1.has_value());
  EXPECT_EQ(*v1, 1u);
  auto doc = b->read_catalog();
  ASSERT_TRUE(doc.has_value() && doc->has_value());
  EXPECT_EQ((*doc)->version, 1u);
  EXPECT_STREQ((*doc)->text, catalog_text(1, "first"));
  EXPECT_STREQ(slurp(dir.path + "/catalog.toml"), catalog_text(1, "first"));
  EXPECT_FALSE(std::filesystem::exists(dir.path + "/catalog.lock"));
  EXPECT_TRUE(a->list_catalog_history()->empty());  // nothing was replaced yet

  // Second commit from the other store object: history holds the replaced content.
  auto v2 = b->write_catalog(1, catalog_text(2, "second"));
  ASSERT_TRUE(v2.has_value());
  EXPECT_EQ(*v2, 2u);
  auto history = a->list_catalog_history();
  ASSERT_TRUE(history.has_value());
  EXPECT_TRUE(*history == std::vector<uint64_t>{1});
  auto old = a->read_catalog_history(1);
  ASSERT_TRUE(old.has_value());
  EXPECT_EQ(old->version, 1u);
  EXPECT_STREQ(old->text, catalog_text(1, "first"));
  auto missing = a->read_catalog_history(2);  // the current version is not history
  ASSERT_TRUE(!missing.has_value());
  EXPECT_TRUE(missing.error() == errno_from(ENOENT));
  // The loser of a race sees EAGAIN and the file is untouched.
  auto lost = a->write_catalog(1, catalog_text(2, "lost"));
  ASSERT_TRUE(!lost.has_value());
  EXPECT_TRUE(lost.error() == errno_from(EAGAIN));
  EXPECT_STREQ(slurp(dir.path + "/catalog.toml"), catalog_text(2, "second"));

  // History is pruned to the newest kCatalogHistoryKeep versions.
  for (uint64_t v = 3; v <= server::kCatalogHistoryKeep + 5; ++v) {
    auto r = (v % 2 ? a : b)->write_catalog(v - 1, catalog_text(v));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, v);
  }
  const uint64_t current = server::kCatalogHistoryKeep + 5;
  EXPECT_EQ((*a->read_catalog())->version, current);
  history = a->list_catalog_history();
  ASSERT_TRUE(history.has_value());
  ASSERT_TRUE(history->size() == server::kCatalogHistoryKeep);
  EXPECT_EQ(history->front(), current - server::kCatalogHistoryKeep);
  EXPECT_EQ(history->back(), current - 1);
  EXPECT_TRUE(!a->read_catalog_history(current - server::kCatalogHistoryKeep - 1).has_value());
  EXPECT_FALSE(has_tmp_leftovers(dir.path));
  EXPECT_FALSE(has_tmp_leftovers(dir.path + "/catalog.history"));

  // A corrupt header is EINVAL for readers and blocks commits until it is repaired.
  write_raw(dir.path + "/catalog.toml", "[catalog]\ncomment = \"no version\"\n");
  auto corrupt = a->read_catalog();
  ASSERT_TRUE(!corrupt.has_value());
  EXPECT_TRUE(corrupt.error() == errno_from(EINVAL));
  auto blocked = a->write_catalog(current, catalog_text(current + 1));
  ASSERT_TRUE(!blocked.has_value());
  EXPECT_TRUE(blocked.error() == errno_from(EINVAL));
  EXPECT_FALSE(std::filesystem::exists(dir.path + "/catalog.lock"));
}

TEST(ClusterStore, CatalogAppliedPerNode) {
  TmpDir dir;
  auto a = server::make_posix_cluster_store(dir.path);
  auto b = server::make_posix_cluster_store(dir.path);
  auto none = a->list_catalog_applied();
  ASSERT_TRUE(none.has_value() && none->empty());

  const int64_t t0 = now_ms();
  ASSERT_TRUE(a->put_catalog_applied({"gw2", 3, "sha256:bbbb", t0, "ok"}).has_value());
  ASSERT_TRUE(b->put_catalog_applied({"gw1", 2, "sha256:aaaa", t0 - 5, "ok"}).has_value());
  // Overwrite, with a status that has spaces in it.
  ASSERT_TRUE(
      a->put_catalog_applied({"gw1", 2, "sha256:aaaa", t0 + 5,
                              "error:merge failed: fsid 3 has no [backend_defaults.cephfs]"})
          .has_value());
  EXPECT_STREQ(slurp(dir.path + "/catalog.gw2"), "3 sha256:bbbb " + std::to_string(t0) + " ok\n");
  auto listed = b->list_catalog_applied();
  ASSERT_TRUE(listed.has_value() && listed->size() == 2u);
  EXPECT_STREQ((*listed)[0].node, "gw1");
  EXPECT_EQ((*listed)[0].version, 2u);
  EXPECT_STREQ((*listed)[0].digest, "sha256:aaaa");
  EXPECT_EQ((*listed)[0].applied_at_ms, t0 + 5);
  EXPECT_STREQ((*listed)[0].status, "error:merge failed: fsid 3 has no [backend_defaults.cephfs]");
  EXPECT_STREQ((*listed)[1].node, "gw2");
  EXPECT_EQ((*listed)[1].version, 3u);
  EXPECT_STREQ((*listed)[1].status, "ok");

  // Not confused with the catalog's own files, and never counted as an export digest
  // or a fence record (the `exports.` / `fence.` prefix filters).
  ASSERT_TRUE(a->write_catalog(0, catalog_text(1)).has_value());
  ASSERT_TRUE(a->write_catalog(1, catalog_text(2)).has_value());
  ASSERT_TRUE(a->put_exports_digest("gw1", "sha256:aaaa").has_value());
  EXPECT_EQ(a->list_catalog_applied()->size(), 2u);
  auto digests = a->list_exports_digests();
  ASSERT_TRUE(digests.has_value());
  EXPECT_EQ(digests->size(), 1u);
  EXPECT_STREQ((*digests)[0].first, "gw1");
  EXPECT_EQ(a->list_fences()->size(), 0u);
  EXPECT_EQ(a->list_nodes()->size(), 0u);
  // Reserved node names and unstorable fields are refused.
  for (const char* node : {"toml", "lock", "history", "", "a/b", "a b"})
    EXPECT_FALSE(a->put_catalog_applied({node, 1, "sha256:x", t0, "ok"}).has_value());
  EXPECT_FALSE(a->put_catalog_applied({"gw3", 1, "sha 256", t0, "ok"}).has_value());
  EXPECT_FALSE(a->put_catalog_applied({"gw3", 1, "sha256:x", t0, ""}).has_value());
  EXPECT_FALSE(a->put_catalog_applied({"gw3", 1, "sha256:x", t0, "ok\nmore"}).has_value());
  EXPECT_EQ(a->list_catalog_applied()->size(), 2u);
  // A corrupt record is an error for the listing; a stray in-flight temp file is skipped.
  write_raw(dir.path + "/catalog.gw3.tmp.1.2", "garbage");
  EXPECT_EQ(a->list_catalog_applied()->size(), 2u);
  write_raw(dir.path + "/catalog.gw3", "3 sha256:cccc\n");
  EXPECT_FALSE(a->list_catalog_applied().has_value());
}

TEST(ClusterStore, MemCatalogMirrorsPosix) {
  // The in-memory double keeps the CAS, history and per-node semantics the controller
  // and ctl tests rely on.
  test::MemClusterStore mem;
  EXPECT_FALSE(mem.read_catalog()->has_value());
  EXPECT_TRUE(mem.write_catalog(1, catalog_text(2)).error() == errno_from(EAGAIN));
  EXPECT_TRUE(mem.write_catalog(0, catalog_text(3)).error() == errno_from(EINVAL));
  EXPECT_EQ(*mem.write_catalog(0, catalog_text(1)), 1u);
  EXPECT_EQ(*mem.write_catalog(1, catalog_text(2)), 2u);
  EXPECT_TRUE(mem.write_catalog(1, catalog_text(2)).error() == errno_from(EAGAIN));
  EXPECT_EQ((*mem.read_catalog())->version, 2u);
  EXPECT_TRUE(*mem.list_catalog_history() == std::vector<uint64_t>{1});
  EXPECT_STREQ(mem.read_catalog_history(1)->text, catalog_text(1));
  EXPECT_TRUE(mem.read_catalog_history(2).error() == errno_from(ENOENT));
  for (uint64_t v = 3; v <= server::kCatalogHistoryKeep + 5; ++v)
    ASSERT_TRUE(mem.write_catalog(v - 1, catalog_text(v)).has_value());
  EXPECT_EQ(mem.list_catalog_history()->size(), server::kCatalogHistoryKeep);
  EXPECT_EQ(mem.list_catalog_history()->front(), 5u);
  mem.fail_write_catalog = errno_from(EIO);
  EXPECT_TRUE(mem.write_catalog(server::kCatalogHistoryKeep + 5, catalog_text(99)).error() ==
              errno_from(EIO));
  ASSERT_TRUE(mem.put_catalog_applied({"gw2", 3, "sha256:b", 1, "ok"}).has_value());
  ASSERT_TRUE(mem.put_catalog_applied({"gw1", 2, "sha256:a", 1, "error:x y"}).has_value());
  EXPECT_FALSE(mem.put_catalog_applied({"toml", 1, "sha256:a", 1, "ok"}).has_value());
  auto listed = mem.list_catalog_applied();
  ASSERT_TRUE(listed.has_value() && listed->size() == 2u);
  EXPECT_STREQ((*listed)[0].node, "gw1");
  EXPECT_STREQ((*listed)[1].status, "ok");
  EXPECT_TRUE(mem.log.back() == "catalog_applied:toml=1");
}
