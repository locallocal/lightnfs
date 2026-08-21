// Local-backend write contract tests (development plan §4.1/§4.3, design 06 §6.2/6.3):
// creation family on a real filesystem, EXCLUSIVE verifier persistence + replay,
// write stability levels + commit, fd-cache read->write upgrade, sticky fsync-EIO
// poisoning, and setattr application.

#include "mini_test.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <condition_variable>
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
