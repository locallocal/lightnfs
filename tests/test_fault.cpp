// Fault-injection hooks (plan doc 10 §7.1): backend/fault.hpp budgets are armed
// programmatically here, exercising the same code paths scripts/fault_inject.sh drives
// via LNFS_FAULT_* env — write ENOSPC/EDQUOT, read EIO, short-write continuation on both
// write overloads, slow-disk sleep, and an injected fsync EIO really poisoning the file.

#include <sys/uio.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include "backend/fault.hpp"
#include "backend/local/local.hpp"
#include "mini_test.hpp"
#include "runtime/runtime.hpp"
#include "util/errno.hpp"

using namespace lnfs;
namespace fs = std::filesystem;
using backend::fault::Kind;

namespace {

template <class T>
T run_runtime(rt::Runtime& runtime, rt::Task<T> task) {
  std::mutex mu;
  std::condition_variable cv;
  std::optional<T> result;
  rt::spawn([](rt::Task<T> work, std::mutex* mu, std::condition_variable* cv,
               std::optional<T>* out) -> rt::Task<void> {
    auto value = co_await std::move(work);
    {
      std::lock_guard lock(*mu);
      out->emplace(std::move(value));
      cv->notify_one();
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
    char tmpl[] = "/tmp/lnfs-fault-XXXXXX";
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

std::span<const std::byte> bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string read_file(const std::string& path) {
  FILE* f = fopen(path.c_str(), "r");
  if (!f) return {};
  char buf[128] = {};
  size_t n = fread(buf, 1, sizeof buf, f);
  fclose(f);
  return std::string(buf, n);
}

}  // namespace

TEST(Fault, InjectedFailuresAreConsumedOnce) {
  backend::fault::clear();
  TmpTree tree;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  {
    auto made = backend::LocalBackend::create({.path = tree.path, .fsid = 5});
    ASSERT_TRUE(made.has_value());
    auto& be = **made;
    auto root = run_runtime(runtime, be.root());
    auto cred = self_cred();
    backend::OpenCtx open{cred, nullptr};
    auto created = run_runtime(runtime, (*root)->create(cred, "f", {}, nullptr));
    ASSERT_TRUE(created.has_value());
    auto& obj = *created->obj;

    // write ENOSPC: exactly one write fails, the next succeeds.
    backend::fault::arm(Kind::kWriteEnospc, 1);
    auto w = run_runtime(runtime, obj.write(open, 0, bytes("x"), backend::Stability::kUnstable));
    ASSERT_TRUE(!w.has_value());
    EXPECT_EQ(raw(w.error()), ENOSPC);
    w = run_runtime(runtime, obj.write(open, 0, bytes("x"), backend::Stability::kUnstable));
    EXPECT_TRUE(w.has_value());

    // write EDQUOT.
    backend::fault::arm(Kind::kWriteEdquot, 1);
    w = run_runtime(runtime, obj.write(open, 0, bytes("x"), backend::Stability::kUnstable));
    ASSERT_TRUE(!w.has_value());
    EXPECT_EQ(raw(w.error()), EDQUOT);

    // read EIO: one read fails, the file stays readable afterwards.
    backend::fault::arm(Kind::kReadEio, 1);
    std::byte out[8];
    bool eof = false;
    auto r = run_runtime(runtime, obj.read(open, 0, std::span<std::byte>(out), eof));
    ASSERT_TRUE(!r.has_value());
    EXPECT_EQ(raw(r.error()), EIO);
    r = run_runtime(runtime, obj.read(open, 0, std::span<std::byte>(out), eof));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 1u);

    // clear() disarms a pending budget.
    backend::fault::arm(Kind::kWriteEnospc, 5);
    backend::fault::clear();
    w = run_runtime(runtime, obj.write(open, 0, bytes("y"), backend::Stability::kUnstable));
    EXPECT_TRUE(w.has_value());
  }
  runtime.stop_and_join();
}

TEST(Fault, ShortWriteContinuationBothOverloads) {
  backend::fault::clear();
  TmpTree tree;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  {
    auto made = backend::LocalBackend::create({.path = tree.path, .fsid = 5});
    ASSERT_TRUE(made.has_value());
    auto cred = self_cred();
    backend::OpenCtx open{cred, nullptr};
    auto root = run_runtime(runtime, (*made)->root());
    auto created = run_runtime(runtime, (*root)->create(cred, "short", {}, nullptr));
    ASSERT_TRUE(created.has_value());
    auto& obj = *created->obj;

    // Span overload: the first 3 uring writes complete 1 byte each; the continuation
    // loop must still deliver the whole buffer at the right offsets.
    backend::fault::arm(Kind::kShortWrite, 3);
    auto w = run_runtime(runtime,
                         obj.write(open, 0, bytes("0123456789"), backend::Stability::kUnstable));
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(*w, 10u);
    EXPECT_STREQ(read_file(tree.path + "/short"), "0123456789");

    // iovec overload: short writes land mid-iovec and across an iovec boundary.
    backend::fault::arm(Kind::kShortWrite, 3);
    const char a[] = "abc";
    const char b[] = "defgh";
    iovec iov[2] = {{const_cast<char*>(a), 3}, {const_cast<char*>(b), 5}};
    auto wv = run_runtime(runtime, obj.write(open, 0, std::span<const iovec>(iov),
                                             backend::Stability::kUnstable));
    ASSERT_TRUE(wv.has_value());
    EXPECT_EQ(*wv, 8u);
    EXPECT_STREQ(read_file(tree.path + "/short").substr(0, 8), "abcdefgh");
  }
  runtime.stop_and_join();
}

TEST(Fault, SlowIoDelaysWithoutFailing) {
  backend::fault::clear();
  TmpTree tree;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  {
    auto made = backend::LocalBackend::create({.path = tree.path, .fsid = 5});
    ASSERT_TRUE(made.has_value());
    auto cred = self_cred();
    backend::OpenCtx open{cred, nullptr};
    auto root = run_runtime(runtime, (*made)->root());
    auto created = run_runtime(runtime, (*root)->create(cred, "slow", {}, nullptr));
    ASSERT_TRUE(created.has_value());

    backend::fault::set_slow_ms(80);
    backend::fault::arm(Kind::kSlowIo, 1);
    auto t0 = std::chrono::steady_clock::now();
    auto w = run_runtime(runtime, created->obj->write(open, 0, bytes("z"),
                                                      backend::Stability::kUnstable));
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_TRUE(w.has_value());
    EXPECT_TRUE(elapsed >= std::chrono::milliseconds(60));
    backend::fault::set_slow_ms(50);
  }
  runtime.stop_and_join();
}

TEST(Fault, InjectedFsyncEioPoisonsFile) {
  backend::fault::clear();
  TmpTree tree;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  {
    auto made = backend::LocalBackend::create({.path = tree.path, .fsid = 5});
    ASSERT_TRUE(made.has_value());
    auto& be = **made;
    auto cred = self_cred();
    backend::OpenCtx open{cred, nullptr};
    auto root = run_runtime(runtime, be.root());
    auto created = run_runtime(runtime, (*root)->create(cred, "sync", {}, nullptr));
    ASSERT_TRUE(created.has_value());

    // A FILE_SYNC write whose fsync fails must poison the file (design 06 §6.2)...
    backend::fault::arm(Kind::kFsyncEio, 1);
    auto w = run_runtime(runtime, created->obj->write(open, 0, bytes("data"),
                                                      backend::Stability::kFileSync));
    ASSERT_TRUE(!w.has_value());
    EXPECT_EQ(raw(w.error()), EIO);
    EXPECT_TRUE(be.is_poisoned(created->obj->id()));
    // ...and COMMIT keeps failing until the operator clears the mark.
    auto c = run_runtime(runtime, created->obj->commit(open, 0, 0));
    ASSERT_TRUE(!c.has_value());
    EXPECT_EQ(raw(c.error()), EIO);
    EXPECT_EQ(be.clear_poison(), 1u);
    EXPECT_TRUE(run_runtime(runtime, created->obj->commit(open, 0, 0)).has_value());
  }
  runtime.stop_and_join();
}
