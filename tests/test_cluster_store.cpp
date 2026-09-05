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
