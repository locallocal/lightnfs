// Local-backend write contract tests (development plan §4.1/§4.3, design 06 §6.2/6.3):
// creation family on a real filesystem, EXCLUSIVE verifier persistence + replay,
// write stability levels + commit, fd-cache read->write upgrade, sticky fsync-EIO
// poisoning, and setattr application.

#include "mini_test.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <vector>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>

#include "backend/local.hpp"
#include "runtime/runtime.hpp"

using namespace lnfs;
namespace fs = std::filesystem;

namespace {

template <class T>
T run_runtime(rt::Runtime& runtime, rt::Task<T> task) {
  std::mutex mu;
  std::condition_variable cv;
  std::optional<T> result;
  rt::spawn(
      [](rt::Task<T> work, std::mutex* mu, std::condition_variable* cv,
         std::optional<T>* out) -> rt::Task<void> {
        auto value = co_await std::move(work);
        {
          std::lock_guard lock(*mu);
          out->emplace(std::move(value));
          cv->notify_one();  // under the lock: the waiter cannot destroy cv first
        }
      }(std::move(task), &mu, &cv, &result),
      runtime.reactor(0));
  std::unique_lock lock(mu);
  cv.wait(lock, [&] { return result.has_value(); });
  return std::move(*result);
}

struct TmpTree {
  std::string path;
  TmpTree() {
    char tmpl[] = "/tmp/lnfs-write-XXXXXX";
    path = mkdtemp(tmpl);
  }
  ~TmpTree() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

backend::Cred self_cred() {
  return backend::Cred{static_cast<uint32_t>(getuid()), static_cast<uint32_t>(getgid()),
                       {}};
}

}  // namespace

TEST(BackendWrite, CreateWriteCommitAndSetattr) {
  TmpTree tree;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  {
    auto made = backend::LocalBackend::create({.path = tree.path, .fsid = 5});
    ASSERT_TRUE(made.has_value());
    auto& be = **made;
    auto root = run_runtime(runtime, be.root());
    ASSERT_TRUE(root.has_value());
    auto cred = self_cred();

    backend::SetAttr attrs;
    attrs.mode = 0640;
    auto created = run_runtime(runtime, (*root)->create(cred, "file", attrs, nullptr));
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(created->attr.mode, 0640u);

    // Write with each stability level, then commit.
    const char msg[] = "stability";
    std::span<const std::byte> data(reinterpret_cast<const std::byte*>(msg), 9);
    backend::OpenCtx open{cred, nullptr};
    auto w1 = run_runtime(runtime,
                          created->obj->write(open, 0, data, backend::Stability::kUnstable));
    ASSERT_TRUE(w1.has_value());
    EXPECT_EQ(*w1, 9u);
    auto w2 = run_runtime(runtime,
                          created->obj->write(open, 9, data, backend::Stability::kDataSync));
    ASSERT_TRUE(w2.has_value());
    auto w3 = run_runtime(runtime,
                          created->obj->write(open, 18, data, backend::Stability::kFileSync));
    ASSERT_TRUE(w3.has_value());
    auto committed = run_runtime(runtime, created->obj->commit(open, 0, 0));
    EXPECT_TRUE(committed.has_value());

    // Verify on-disk content directly.
    std::string on_disk;
    {
      FILE* f = fopen((tree.path + "/file").c_str(), "r");
      char buf[64] = {};
      size_t n = fread(buf, 1, sizeof buf, f);
      fclose(f);
      on_disk.assign(buf, n);
    }
    EXPECT_STREQ(on_disk, "stabilitystabilitystability");

    // setattr: truncate + chmod + client mtime.
    backend::SetAttr set;
    set.size = 9;
    set.mode = 0600;
    set.mtime_how = backend::SetAttr::TimeHow::kClient;
    set.mtime = {1234567, 0};
    auto after = run_runtime(runtime, created->obj->setattr(cred, set));
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->size, 9u);
    EXPECT_EQ(after->mode, 0600u);
    EXPECT_EQ(after->mtime.sec, 1234567);

    // Sticky poisoning: after a recorded sync failure, commit must keep failing.
    be.poison(created->obj->id());
    auto poisoned = run_runtime(runtime, created->obj->commit(open, 0, 0));
    ASSERT_TRUE(!poisoned.has_value());
    EXPECT_EQ(raw(poisoned.error()), EIO);
    // Operator override (plan doc 10 §1.5): clearing the marks makes COMMIT work
    // again without a process restart.
    EXPECT_EQ(be.clear_poison(), 1u);
    EXPECT_TRUE(!be.is_poisoned(created->obj->id()));
    auto healed = run_runtime(runtime, created->obj->commit(open, 0, 0));
    EXPECT_TRUE(healed.has_value());
  }
  runtime.stop_and_join();
}

TEST(BackendWrite, ExclusiveVerifierPersistsAcrossBackendRestart) {
  TmpTree tree;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  {
    backend::ExclVerf verf{};
    verf[0] = std::byte{0x42};
    verf[7] = std::byte{0x99};
    auto cred = self_cred();
    {
      auto made = backend::LocalBackend::create({.path = tree.path, .fsid = 5});
      ASSERT_TRUE(made.has_value());
      auto root = run_runtime(runtime, (*made)->root());
      auto created = run_runtime(runtime, (*root)->create(cred, "excl", {}, &verf));
      ASSERT_TRUE(created.has_value());
    }
    {
      // New backend instance = server restart; the retransmitted EXCLUSIVE create must
      // still match the verifier persisted in atime/mtime.
      auto made = backend::LocalBackend::create({.path = tree.path, .fsid = 5});
      ASSERT_TRUE(made.has_value());
      auto root = run_runtime(runtime, (*made)->root());
      auto replay = run_runtime(runtime, (*root)->create(cred, "excl", {}, &verf));
      EXPECT_TRUE(replay.has_value());

      backend::ExclVerf other{};
      other[0] = std::byte{0x43};
      auto conflict = run_runtime(runtime, (*root)->create(cred, "excl", {}, &other));
      ASSERT_TRUE(!conflict.has_value());
      EXPECT_EQ(raw(conflict.error()), EEXIST);
    }
  }
  runtime.stop_and_join();
}

TEST(BackendWrite, NamespaceOpsAndFdCacheUpgrade) {
  TmpTree tree;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  {
    auto made = backend::LocalBackend::create({.path = tree.path, .fsid = 5});
    ASSERT_TRUE(made.has_value());
    auto& be = **made;
    auto root = run_runtime(runtime, be.root());
    auto cred = self_cred();

    // mkdir / symlink / rename / link / unlink / rmdir
    auto dir = run_runtime(runtime, (*root)->mkdir(cred, "dir", {}));
    ASSERT_TRUE(dir.has_value());
    auto file = run_runtime(runtime, (*root)->create(cred, "a", {}, nullptr));
    ASSERT_TRUE(file.has_value());
    auto sym = run_runtime(runtime, (*root)->symlink(cred, "s", "a", {}));
    ASSERT_TRUE(sym.has_value());
    auto target = run_runtime(runtime, sym->obj->readlink());
    ASSERT_TRUE(target.has_value());
    EXPECT_STREQ(*target, "a");

    auto renamed =
        run_runtime(runtime, (*root)->rename(cred, "a", *dir->obj, "b"));
    EXPECT_TRUE(renamed.has_value());
    EXPECT_TRUE(fs::exists(tree.path + "/dir/b"));
    EXPECT_TRUE(!fs::exists(tree.path + "/a"));

    auto relinked = run_runtime(runtime, (*root)->link(cred, *file->obj, "hard"));
    EXPECT_TRUE(relinked.has_value());
    struct stat st{};
    ASSERT_TRUE(::stat((tree.path + "/hard").c_str(), &st) == 0);
    EXPECT_EQ(st.st_nlink, 2u);

    auto unlinked = run_runtime(runtime, (*root)->unlink(cred, "hard"));
    EXPECT_TRUE(unlinked.has_value());
    auto not_empty = run_runtime(runtime, (*root)->rmdir(cred, "dir"));
    ASSERT_TRUE(!not_empty.has_value());
    EXPECT_EQ(raw(not_empty.error()), ENOTEMPTY);
    auto gone = run_runtime(runtime, dir->obj->unlink(cred, "b"));
    EXPECT_TRUE(gone.has_value());
    auto rmdir_ok = run_runtime(runtime, (*root)->rmdir(cred, "dir"));
    EXPECT_TRUE(rmdir_ok.has_value());

    // fd cache: read first (O_RDONLY entry), then write (upgrade to O_RDWR).
    auto rw = run_runtime(runtime, (*root)->create(cred, "rw", {}, nullptr));
    ASSERT_TRUE(rw.has_value());
    backend::OpenCtx open{cred, nullptr};
    const char msg[] = "x";
    std::span<const std::byte> one(reinterpret_cast<const std::byte*>(msg), 1);
    auto w = run_runtime(runtime, rw->obj->write(open, 0, one, backend::Stability::kFileSync));
    ASSERT_TRUE(w.has_value());
    std::array<std::byte, 4> buf{};
    bool eof = false;
    auto task = [&]() -> rt::Task<Result<uint32_t>> {
      co_return co_await rw->obj->read(open, 0, std::span<std::byte>(buf.data(), 4), eof);
    };
    auto r = run_runtime(runtime, task());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 1u);
    auto stats = be.fd_cache_stats();
    EXPECT_TRUE(stats.misses >= 1);
  }
  runtime.stop_and_join();
}

// Plan doc 10 §1.3: the fd-cache capacity is a hard cap. Idle entries past capacity are
// evicted on insert (LRU), and a shard whose entries are all pinned by in-flight IO is
// counted as an overflow instead of growing silently toward RLIMIT_NOFILE.
TEST(BackendWrite, FdCacheHardCapAndOverflow) {
  TmpTree tree;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  {
    // fd_cache = 16 spreads to one entry per shard (the cache keeps 16 shards).
    auto made = backend::LocalBackend::create(
        {.path = tree.path, .fsid = 5, .fd_cache = 16});
    ASSERT_TRUE(made.has_value());
    auto& be = **made;
    auto root = run_runtime(runtime, be.root());
    ASSERT_TRUE(root.has_value());
    auto cred = self_cred();
    backend::OpenCtx open{cred, nullptr};
    const char msg[] = "y";
    std::span<const std::byte> one(reinterpret_cast<const std::byte*>(msg), 1);

    // 64 sequential single writes: nothing stays pinned between ops, so the cache must
    // never exceed one entry per shard no matter how many distinct files flow through.
    std::vector<backend::ObjPtr> files;
    for (int i = 0; i < 64; ++i) {
      auto f = run_runtime(runtime,
                           (*root)->create(cred, "f" + std::to_string(i), {}, nullptr));
      ASSERT_TRUE(f.has_value());
      auto w = run_runtime(runtime,
                           f->obj->write(open, 0, one, backend::Stability::kUnstable));
      ASSERT_TRUE(w.has_value());
      files.push_back(f->obj);
    }
    auto stats = be.fd_cache_stats();
    EXPECT_TRUE(stats.entries <= 16);
    EXPECT_TRUE(stats.evictions >= 64 - 16);
    EXPECT_EQ(stats.overflows, 0u);

    // The last-written file survived its own insert's eviction pass: cached-fd hit.
    auto again = run_runtime(
        runtime, files.back()->write(open, 0, one, backend::Stability::kUnstable));
    ASSERT_TRUE(again.has_value());
    EXPECT_TRUE(be.fd_cache_stats().hits > stats.hits);

    // Overflow: copy_range pins a source and a destination fd at once. Pick two files
    // in the same shard (64 files over 16 shards guarantees a collision) so both pinned
    // entries land in a shard with capacity one — the pass finds nothing evictable.
    backend::ObjPtr src, dst;
    for (size_t i = 0; i < files.size() && !dst; ++i)
      for (size_t j = i + 1; j < files.size() && !dst; ++j)
        if (backend::ObjIdHash{}(files[i]->id()) % 16 ==
            backend::ObjIdHash{}(files[j]->id()) % 16) {
          src = files[i];
          dst = files[j];
        }
    ASSERT_TRUE(src && dst);
    auto copied =
        run_runtime(runtime, src->copy_range(open, *dst, open, 0, 0, 1));
    ASSERT_TRUE(copied.has_value());
    EXPECT_TRUE(be.fd_cache_stats().overflows >= 1);
  }
  runtime.stop_and_join();
}

// Phase 6 (development plan §8 item 1): the v4.2 backend contract on a real filesystem.
// Capability bits come from the startup probe; the test asserts behavior consistent with
// them (tmpfs/ext4: sparse + copy yes; clone only on XFS-reflink/Btrfs).
TEST(BackendWrite, V42SparseCopyClone) {
  TmpTree tree;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  {
    auto made = backend::LocalBackend::create({.path = tree.path, .fsid = 5});
    ASSERT_TRUE(made.has_value());
    auto& be = **made;
    auto caps = be.caps();
    EXPECT_TRUE(caps.has(backend::Cap::kCopyRange));  // pread/pwrite fallback always
    auto root = run_runtime(runtime, be.root());
    ASSERT_TRUE(root.has_value());
    auto cred = self_cred();
    backend::OpenCtx open{cred, nullptr};

    backend::SetAttr attrs;
    attrs.mode = 0644;
    auto src = run_runtime(runtime, (*root)->create(cred, "src", attrs, nullptr));
    auto dst = run_runtime(runtime, (*root)->create(cred, "dst", attrs, nullptr));
    ASSERT_TRUE(src.has_value() && dst.has_value());
    std::vector<std::byte> block(1 << 16);
    for (size_t i = 0; i < block.size(); ++i) block[i] = static_cast<std::byte>(i * 7);
    // Two 64 KiB data blocks with a 64 KiB gap written as zeroes by extension.
    ASSERT_TRUE(run_runtime(runtime, src->obj->write(open, 0, block, backend::Stability::kUnstable)).has_value());
    ASSERT_TRUE(run_runtime(runtime, src->obj->write(open, 2 << 16, block, backend::Stability::kFileSync)).has_value());

    if (caps.has(backend::Cap::kSparseOps)) {
      // Punch the middle block: size unchanged, reads zero, SEEK reports the hole.
      auto punched = run_runtime(runtime, src->obj->deallocate(open, 1 << 16, 1 << 16));
      ASSERT_TRUE(punched.has_value());
      auto attr = run_runtime(runtime, src->obj->getattr());
      ASSERT_TRUE(attr.has_value());
      EXPECT_EQ(attr->size, 3u << 16);
      auto hole = run_runtime(runtime, src->obj->seek(open, 0, backend::SeekWhat::kHole));
      ASSERT_TRUE(hole.has_value());
      EXPECT_TRUE(*hole >= (1u << 16) && *hole <= (2u << 16));  // fs granularity
      auto data = run_runtime(runtime, src->obj->seek(open, 1 << 16, backend::SeekWhat::kData));
      ASSERT_TRUE(data.has_value());
      EXPECT_TRUE(*data >= (1u << 16) && *data <= (2u << 16));
      auto past = run_runtime(runtime, src->obj->seek(open, 3 << 16, backend::SeekWhat::kData));
      ASSERT_TRUE(!past.has_value());
      EXPECT_EQ(raw(past.error()), ENXIO);
      // ALLOCATE past EOF extends the file.
      auto grown = run_runtime(runtime, src->obj->allocate(open, 3 << 16, 4096));
      ASSERT_TRUE(grown.has_value());
      attr = run_runtime(runtime, src->obj->getattr());
      EXPECT_EQ(attr->size, (3u << 16) + 4096);
      ASSERT_TRUE(run_runtime(runtime, src->obj->setattr(cred, backend::SetAttr{.size = 3u << 16})).has_value());
    }

    // copy_range: whole range through EOF (len 0) and a ranged copy at an offset.
    auto copied = run_runtime(runtime, src->obj->copy_range(open, *dst->obj, open, 0, 0, 0));
    ASSERT_TRUE(copied.has_value());
    EXPECT_EQ(*copied, 3u << 16);
    auto ranged = run_runtime(runtime, src->obj->copy_range(open, *dst->obj, open, 2 << 16, 3 << 16, 100));
    ASSERT_TRUE(ranged.has_value());
    EXPECT_EQ(*ranged, 100u);
    {
      std::vector<std::byte> a((3u << 16) + 100), b((3u << 16) + 100);
      bool eof = false;
      auto ra = run_runtime(runtime, src->obj->read(open, 0, a, eof));
      auto rb = run_runtime(runtime, dst->obj->read(open, 0, b, eof));
      ASSERT_TRUE(ra.has_value() && rb.has_value());
      EXPECT_EQ(*ra, 3u << 16);
      EXPECT_EQ(*rb, (3u << 16) + 100);
      EXPECT_TRUE(std::equal(a.begin(), a.begin() + (3u << 16), b.begin()));
      EXPECT_TRUE(std::equal(b.begin() + (3u << 16), b.end(), a.begin() + (2u << 16)));
    }
    // clone: honored iff the probe said so; otherwise the kernel refuses and the errno
    // is one the CLONE whitelist carries (NOTSUPP / INVAL / XDEV).
    auto cl = run_runtime(runtime, (*root)->create(cred, "clone", attrs, nullptr));
    ASSERT_TRUE(cl.has_value());
    auto cloned = run_runtime(runtime, src->obj->clone(open, *cl->obj, open, 0, 0, 0));
    if (caps.has(backend::Cap::kCloneRange)) {
      ASSERT_TRUE(cloned.has_value());
      auto attr = run_runtime(runtime, cl->obj->getattr());
      EXPECT_EQ(attr->size, 3u << 16);
    } else {
      ASSERT_TRUE(!cloned.has_value());
      int e = raw(cloned.error());
      EXPECT_TRUE(e == EOPNOTSUPP || e == EINVAL || e == EXDEV);
    }
    // Cross-backend copy/clone is EXDEV at the boundary.
    auto other = backend::LocalBackend::create({.path = tree.path, .fsid = 6});
    ASSERT_TRUE(other.has_value());
    auto oroot = run_runtime(runtime, (*other)->root());
    auto far = run_runtime(runtime, (*oroot)->create(cred, "far", attrs, nullptr));
    ASSERT_TRUE(far.has_value());
    auto xdev = run_runtime(runtime, src->obj->copy_range(open, *far->obj, open, 0, 0, 1));
    ASSERT_TRUE(!xdev.has_value());
    EXPECT_EQ(raw(xdev.error()), EXDEV);
  }
  runtime.stop_and_join();
}

// Plan doc 10 §2.4: the scatter write hands the payload to the kernel as segments
// (IORING_OP_WRITEV) — bytes must land exactly as if flattened, including the fsync
// tail for stable writes.
TEST(BackendWrite, ScatterWriteVectored) {
  TmpTree tree;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  {
    auto made = backend::LocalBackend::create({.path = tree.path, .fsid = 5});
    ASSERT_TRUE(made.has_value());
    auto& be = **made;
    auto root = run_runtime(runtime, be.root());
    ASSERT_TRUE(root.has_value());
    auto cred = self_cred();
    auto created =
        run_runtime(runtime, (*root)->create(cred, "vfile", backend::SetAttr{}, nullptr));
    ASSERT_TRUE(created.has_value());
    backend::OpenCtx open{cred, nullptr};

    const char a[] = "hello ";
    const char b[] = "scatter ";
    const char c[] = "world";
    iovec iov[3] = {{const_cast<char*>(a), 6}, {const_cast<char*>(b), 8},
                    {const_cast<char*>(c), 5}};
    auto w = run_runtime(runtime,
                         created->obj->write(open, 0, std::span<const iovec>(iov, 3),
                                             backend::Stability::kFileSync));
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(*w, 19u);

    std::string on_disk;
    {
      FILE* f = fopen((tree.path + "/vfile").c_str(), "r");
      char buf[64] = {};
      size_t n = fread(buf, 1, sizeof buf, f);
      fclose(f);
      on_disk.assign(buf, n);
    }
    EXPECT_STREQ(on_disk, "hello scatter world");

    // Offset + empty segments: writev skips zero-length iovecs.
    iovec iov2[3] = {{const_cast<char*>(a), 0}, {const_cast<char*>(c), 5},
                     {const_cast<char*>(a), 0}};
    auto w2 = run_runtime(runtime,
                          created->obj->write(open, 19, std::span<const iovec>(iov2, 3),
                                              backend::Stability::kUnstable));
    ASSERT_TRUE(w2.has_value());
    EXPECT_EQ(*w2, 5u);
  }
  runtime.stop_and_join();
}

// plan doc 10 §7.1: FICLONERANGE requires block-aligned offsets; the backend must pass
// the kernel's EINVAL through untouched.  Needs a reflink filesystem (Btrfs/XFS) — on
// others kCloneRange is not advertised and the case self-skips (CI covers reflink).
TEST(BackendWrite, CloneMisalignedEinvalPassthrough) {
  char cwd_tmpl[] = "lnfs-clone-XXXXXX";
  char tmp_tmpl[] = "/tmp/lnfs-clone-XXXXXX";
  char* dir = mkdtemp(cwd_tmpl);
  std::string path = fs::absolute(dir ? dir : mkdtemp(tmp_tmpl)).string();
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  {
    auto made = backend::LocalBackend::create({.path = path, .fsid = 7});
    ASSERT_TRUE(made.has_value());
    auto& be = **made;
    if (!be.caps().has(backend::Cap::kCloneRange)) {
      std::printf("  note: no reflink support on %s — CLONE EINVAL passthrough not "
                  "reachable here\n", path.c_str());
    } else {
      auto cred = self_cred();
      backend::OpenCtx open{cred, nullptr};
      auto root = run_runtime(runtime, be.root());
      ASSERT_TRUE(root.has_value());
      auto src = run_runtime(runtime, (*root)->create(cred, "src", {}, nullptr));
      auto dst = run_runtime(runtime, (*root)->create(cred, "dst", {}, nullptr));
      ASSERT_TRUE(src.has_value() && dst.has_value());
      std::vector<std::byte> block(8192, std::byte{0x5a});
      auto w = run_runtime(runtime, src->obj->write(open, 0, std::span<const std::byte>(block),
                                                    backend::Stability::kFileSync));
      ASSERT_TRUE(w.has_value());
      // Aligned clone works; a 1-byte source offset violates FICLONERANGE's block
      // alignment and the kernel's EINVAL must surface (not EOPNOTSUPP, not success).
      auto ok = run_runtime(runtime, src->obj->clone(open, *dst->obj, open, 0, 0, 4096));
      EXPECT_TRUE(ok.has_value());
      auto bad = run_runtime(runtime, src->obj->clone(open, *dst->obj, open, 1, 0, 4096));
      ASSERT_TRUE(!bad.has_value());
      EXPECT_EQ(raw(bad.error()), EINVAL);
    }
  }
  runtime.stop_and_join();
  std::error_code ec;
  fs::remove_all(path, ec);
}
