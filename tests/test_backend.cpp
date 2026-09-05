#include "mini_test.hpp"

#include <arpa/inet.h>

#include <array>
#include <condition_variable>
#include <filesystem>
#include <fcntl.h>
#include <mutex>
#include <unordered_set>
#include <unistd.h>

#include "backend/memory/memory.hpp"
#include "backend/local/local.hpp"
#include "core/config.hpp"
#include "core/file_handle.hpp"
#include "core/readdir.hpp"
#include "runtime/reactor.hpp"
#include "runtime/runtime.hpp"
#include "runtime/testing/fake_ring.hpp"

using namespace lnfs;

namespace {

template <class T>
T run_immediate(rt::Reactor& reactor, rt::Task<T> task) {
  std::optional<T> result;
  rt::spawn(
      [](rt::Task<T> work, std::optional<T>* out) -> rt::Task<void> {
        out->emplace(co_await std::move(work));
      }(std::move(task), &result),
      reactor);
  while (!result) reactor.poll_once();
  return std::move(*result);
}

sockaddr_storage loopback() {
  sockaddr_storage out{};
  auto* addr = reinterpret_cast<sockaddr_in*>(&out);
  addr->sin_family = AF_INET;
  inet_pton(AF_INET, "127.0.0.1", &addr->sin_addr);
  return out;
}

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

}  // namespace

TEST(Backend, MemoryLookupReadAndStableCookies) {
  backend::MemoryBackend memory(7);
  ASSERT_TRUE(memory.add_dir("/dir").has_value());
  ASSERT_TRUE(memory.add_file("/dir/a", "alpha").has_value());
  ASSERT_TRUE(memory.add_file("/dir/b", "beta").has_value());
  rt::testing::FakeRing ring;
  rt::Reactor reactor(ring);
  auto root = run_immediate(reactor, memory.root());
  ASSERT_TRUE(root.has_value());
  backend::Cred cred{0, 0, {}};
  auto dir = run_immediate(reactor, (*root)->lookup(cred, "dir"));
  ASSERT_TRUE(dir.has_value());
  auto first = run_immediate(reactor, core::readdir_page(*dir, cred, 0, 3));
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->ents.size(), 3u);  // ., .., a
  EXPECT_STREQ(first->ents[0].name, ".");
  EXPECT_STREQ(first->ents[1].name, "..");
  uint64_t cookie = first->ents.back().cookie;
  auto second = run_immediate(reactor, core::readdir_page(*dir, cred, cookie, 8));
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->ents.size(), 1u);
  EXPECT_STREQ(second->ents[0].name, "b");
  EXPECT_TRUE(second->eof);

  auto file = run_immediate(reactor, (*dir)->lookup(cred, "a"));
  ASSERT_TRUE(file.has_value());
  std::array<std::byte, 16> data{};
  bool eof = false;
  auto n = run_immediate(reactor,
                         (*file)->read(backend::OpenCtx{cred}, 0, data, eof));
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(*n, 5u);
  EXPECT_TRUE(eof);
  EXPECT_STREQ(std::string(reinterpret_cast<char*>(data.data()), *n), "alpha");
}

TEST(Backend, DefaultTakeoverIsANoOpThatSucceeds) {
  // plan 10 D1: Backend::takeover() is optional — the base implementation succeeds
  // without touching anything, so every backend is cluster-callable by default.
  backend::MemoryBackend memory(7);
  rt::testing::FakeRing ring;
  rt::Reactor reactor(ring);
  backend::ClusterIdentity id{.cluster_id = "cluster-x", .node = "gw1", .epoch = 3};
  auto took = run_immediate(reactor, memory.takeover(id));
  EXPECT_TRUE(took.has_value());
  auto root = run_immediate(reactor, memory.root());
  ASSERT_TRUE(root.has_value());  // still serving afterwards
}

TEST(Backend, HundredThousandEntryTraversalHasNoDuplicatesOrOmissions) {
  backend::MemoryBackend memory(8);
  ASSERT_TRUE(memory.add_dir("/big").has_value());
  constexpr uint32_t kEntries = 100000;
  for (uint32_t i = 0; i < kEntries; ++i)
    ASSERT_TRUE(memory.add_file("/big/f" + std::to_string(i), "").has_value());
  rt::testing::FakeRing ring;
  rt::Reactor reactor(ring);
  backend::Cred cred{0, 0, {}};
  auto root = run_immediate(reactor, memory.root());
  ASSERT_TRUE(root.has_value());
  auto dir = run_immediate(reactor, (*root)->lookup(cred, "big"));
  ASSERT_TRUE(dir.has_value());
  std::unordered_set<uint64_t> seen;
  uint64_t cookie = 0;
  bool eof = false;
  while (!eof) {
    auto page = run_immediate(reactor, (*dir)->readdir(cred, cookie, 4096));
    ASSERT_TRUE(page.has_value());
    for (const auto& ent : page->ents) {
      EXPECT_TRUE(ent.cookie > cookie);
      EXPECT_TRUE(seen.insert(ent.fileid).second);
      cookie = ent.cookie;
    }
    eof = page->eof;
  }
  EXPECT_EQ(seen.size(), static_cast<size_t>(kEntries));
}

TEST(Backend, LocalFallbackReadDirectoryAndStaleHandle) {
  char path_template[] = "/tmp/lightnfs-local-XXXXXX";
  char* path = mkdtemp(path_template);
  ASSERT_TRUE(path != nullptr);
  std::string file_path = std::string(path) + "/data";
  int fd = open(file_path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
  ASSERT_TRUE(fd >= 0);
  ASSERT_TRUE(write(fd, "payload", 7) == 7);
  close(fd);
  std::string link_path = std::string(path) + "/link";
  ASSERT_TRUE(symlink("data", link_path.c_str()) == 0);

  backend::LocalBackend::Config cfg{.path = path,
                                     .fsid = 31,
                                     .fd_cache = 8,
                                     .handles = backend::LocalBackend::HandleMode::kFallback};
  auto made = backend::LocalBackend::create(cfg);
  ASSERT_TRUE(made.has_value());
  auto backend = std::move(*made);
  EXPECT_FALSE(backend->caps().has(backend::Cap::kStableHandles));
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2, .ring = "epoll"});
  runtime.start();
  backend::Cred cred{0, 0, {}};
  auto root = run_runtime(runtime, backend->root());
  ASSERT_TRUE(root.has_value());
  backend::ObjId root_id = (*root)->id();
  auto file = run_runtime(runtime, (*root)->lookup(cred, "data"));
  ASSERT_TRUE(file.has_value());
  backend::ObjId old_id = (*file)->id();
  std::array<std::byte, 16> bytes{};
  bool eof = false;
  auto read = run_runtime(
      runtime, (*file)->read(backend::OpenCtx{cred}, 0, bytes, eof));
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(*read, 7u);
  EXPECT_TRUE(eof);
  EXPECT_STREQ(std::string(reinterpret_cast<char*>(bytes.data()), *read), "payload");
  fd = open(file_path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
  ASSERT_TRUE(fd >= 0);
  ASSERT_TRUE(write(fd, "!", 1) == 1);
  close(fd);
  auto changed = run_runtime(runtime, (*file)->getattr());
  ASSERT_TRUE(changed.has_value());
  EXPECT_EQ(changed->size, 8u);  // getattr must not serve a stale cached size.

  auto page = run_runtime(runtime, (*root)->readdir(cred, 0, 1));
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(page->ents.size(), 1u);
  std::unordered_set<std::string> names{page->ents[0].name};
  uint64_t dir_cookie = page->ents[0].cookie;
  std::string added_path = std::string(path) + "/added";
  fd = open(added_path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
  ASSERT_TRUE(fd >= 0);
  close(fd);
  bool dir_eof = page->eof;
  while (!dir_eof) {
    page = run_runtime(runtime, (*root)->readdir(cred, dir_cookie, 16));
    ASSERT_TRUE(page.has_value());
    for (const auto& ent : page->ents) {
      EXPECT_TRUE(names.insert(ent.name).second);
      dir_cookie = ent.cookie;
    }
    dir_eof = page->eof;
  }
  EXPECT_TRUE(names.contains("data"));
  EXPECT_TRUE(names.contains("link"));
  auto link = run_runtime(runtime, (*root)->lookup(cred, "link"));
  ASSERT_TRUE(link.has_value());
  auto target = run_runtime(runtime, (*link)->readlink());
  ASSERT_TRUE(target.has_value());
  EXPECT_STREQ(*target, "data");

  ASSERT_TRUE(unlink(file_path.c_str()) == 0);
  fd = open(file_path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
  ASSERT_TRUE(fd >= 0);
  close(fd);
  auto stale = run_runtime(runtime, backend->resolve(old_id));
  EXPECT_FALSE(stale.has_value());
  EXPECT_EQ((int)stale.error(), ESTALE);
  auto recreated = run_runtime(runtime, (*root)->lookup(cred, "data"));
  ASSERT_TRUE(recreated.has_value());
  EXPECT_FALSE((*recreated)->id() == old_id);
  backend::ObjId recreated_id = (*recreated)->id();

  runtime.stop_and_join();
  recreated = Err(errno_from(ESTALE));
  link = Err(errno_from(ESTALE));
  file = Err(errno_from(ESTALE));
  root = Err(errno_from(ESTALE));
  backend.reset();

  // Rebuilding the fallback backend reproduces ObjIds from inode+btime.  Its documented
  // path index is process-local, so lookup repopulates the reverse map before resolve.
  auto restarted_made = backend::LocalBackend::create(cfg);
  ASSERT_TRUE(restarted_made.has_value());
  auto restarted = std::move(*restarted_made);
  rt::Runtime runtime2({.reactors = 1, .offload_threads = 2, .ring = "epoll"});
  runtime2.start();
  auto root2 = run_runtime(runtime2, restarted->root());
  ASSERT_TRUE(root2.has_value());
  EXPECT_TRUE((*root2)->id() == root_id);
  auto recreated2 = run_runtime(runtime2, (*root2)->lookup(cred, "data"));
  ASSERT_TRUE(recreated2.has_value());
  EXPECT_TRUE((*recreated2)->id() == recreated_id);
  auto resolved2 = run_runtime(runtime2, restarted->resolve(recreated_id));
  ASSERT_TRUE(resolved2.has_value());
  runtime2.stop_and_join();
  resolved2 = Err(errno_from(ESTALE));
  recreated2 = Err(errno_from(ESTALE));
  root2 = Err(errno_from(ESTALE));
  restarted.reset();
  std::filesystem::remove_all(path);
}

TEST(Core, ConfigCidrSquashAndValidation) {
  std::string text = R"(
[server]
port = 2049
mount_port = 20048
state_dir = "/tmp/lightnfs-test"
max_request_size = "2MiB"

[[export]]
path = "/tmp"
backend = "local"
fsid = 9
clients = ["127.0.0.0/8"]
squash = "root"
readonly = true
[export.local]
handles = "fallback"
fd_cache = 32
)";
  auto parsed = core::parse_config(text);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->server.max_request_size, 2u << 20);
  EXPECT_EQ(parsed->exports.size(), 1u);
  EXPECT_TRUE(core::validate_config(*parsed).has_value());
  auto cidr = core::Cidr::parse("127.0.0.0/8");
  ASSERT_TRUE(cidr.has_value());
  EXPECT_TRUE(cidr->contains(loopback()));
  sockaddr_storage mapped{};
  auto* mapped6 = reinterpret_cast<sockaddr_in6*>(&mapped);
  mapped6->sin6_family = AF_INET6;
  inet_pton(AF_INET6, "::ffff:127.0.0.1", &mapped6->sin6_addr);
  EXPECT_TRUE(cidr->contains(mapped));
  EXPECT_FALSE(core::Cidr::parse("127.0.0.1/99").has_value());
}

TEST(Core, LocalBackendConfigKeysReachFactory) {
  char tmpl[] = "/tmp/lnfs-cfgkeys-XXXXXX";
  ASSERT_TRUE(::mkdtemp(tmpl) != nullptr);
  std::string dir = tmpl;
  auto config_text = [&](std::string_view backend_table) {
    return "[server]\nstate_dir = \"/tmp/lightnfs-test\"\n\n[[export]]\npath = \"" + dir +
           "\"\nbackend = \"local\"\nfsid = 9\nclients = [\"127.0.0.0/8\"]\n"
           "[export.local]\n" +
           std::string(backend_table);
  };

  // Every documented [export.local] key must reach the backend: the build path used to
  // special-case "local" and silently drop identity/readdir_enrich.
  auto parsed = core::parse_config(config_text("handles = \"fallback\"\nfd_cache = 123\n"
                                               "readdir_enrich = false\nidentity = \"strict\"\n"));
  ASSERT_TRUE(parsed.has_value());
  auto table = core::ExportTable::build(std::move(*parsed));
  ASSERT_TRUE(table.has_value());
  auto* exp = (*table)->by_fsid(9);
  ASSERT_TRUE(exp != nullptr);
  auto* local = dynamic_cast<backend::LocalBackend*>(exp->backend.get());
  ASSERT_TRUE(local != nullptr);
  EXPECT_EQ(local->config().fd_cache, 123u);
  EXPECT_TRUE(local->config().handles == backend::LocalBackend::HandleMode::kFallback);
  EXPECT_TRUE(local->config().identity == backend::LocalBackend::Identity::kStrict);
  EXPECT_TRUE(!local->config().enrich_readdir);

  // Bad values and unknown keys fail the build instead of being silently ignored (and a
  // non-numeric fd_cache must not throw out of ExportTable::build).
  auto bad_number = core::parse_config(config_text("fd_cache = \"abc\"\n"));
  ASSERT_TRUE(bad_number.has_value());
  EXPECT_FALSE(core::ExportTable::build(std::move(*bad_number)).has_value());
  auto bad_value = core::parse_config(config_text("identity = \"strck\"\n"));
  ASSERT_TRUE(bad_value.has_value());
  EXPECT_FALSE(core::ExportTable::build(std::move(*bad_value)).has_value());
  auto unknown_key = core::parse_config(config_text("bogus = true\n"));
  ASSERT_TRUE(unknown_key.has_value());
  EXPECT_FALSE(core::ExportTable::build(std::move(*unknown_key)).has_value());

  std::filesystem::remove_all(dir);
}

TEST(Core, FileHandleAuthenticatesAndClassifiesFailures) {
  core::ExportTable table;
  core::ExportConfig cfg;
  cfg.path = "/mem";
  cfg.fsid = 17;
  cfg.clients = {"127.0.0.0/8"};
  auto memory = std::make_unique<backend::MemoryBackend>(17);
  backend::MemoryBackend* raw = memory.get();
  ASSERT_TRUE(table.add(cfg, std::move(memory)).has_value());
  std::array<std::byte, 16> key{};
  for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::byte>(i);
  auto codec = core::FileHandleCodec::from_key(key, table);
  rt::testing::FakeRing ring;
  rt::Reactor reactor(ring);
  auto root = run_immediate(reactor, raw->root());
  ASSERT_TRUE(root.has_value());
  auto* exp = table.by_fsid(17);
  auto fh = codec.encode(*exp, (*root)->id());
  auto decoded = codec.decode(fh, loopback());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->oid == (*root)->id());
  sockaddr_storage denied{};
  auto* denied4 = reinterpret_cast<sockaddr_in*>(&denied);
  denied4->sin_family = AF_INET;
  inet_pton(AF_INET, "10.0.0.1", &denied4->sin_addr);
  auto inaccessible = codec.decode(fh, denied);
  EXPECT_FALSE(inaccessible.has_value());
  EXPECT_EQ((int)inaccessible.error(), EACCES);
  core::ExportTable empty;
  auto stale_codec = core::FileHandleCodec::from_key(key, empty);
  auto stale = stale_codec.decode(fh, loopback());
  EXPECT_FALSE(stale.has_value());
  EXPECT_EQ((int)stale.error(), ESTALE);
  fh[6] ^= std::byte{1};
  auto forged = codec.decode(fh, loopback());
  EXPECT_FALSE(forged.has_value());
  EXPECT_EQ((int)forged.error(), (int)Errno::kBadHandle);
}

// Plan doc 10 §1.5: fallback-mode handle bookkeeping is hard-capped. Enumerating a big
// tree used to leave one path string per entry forever; past the cap old handles go
// ESTALE instead of the maps growing without bound.
TEST(Backend, LocalFallbackPathTableIsCapped) {
  char path_template[] = "/tmp/lightnfs-cap-XXXXXX";
  char* path = mkdtemp(path_template);
  ASSERT_TRUE(path != nullptr);
  for (int i = 0; i < 12; ++i) {
    std::string p = std::string(path) + "/f" + std::to_string(i);
    int fd = open(p.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    ASSERT_TRUE(fd >= 0);
    close(fd);
  }
  backend::LocalBackend::Config cfg{.path = path,
                                    .fsid = 32,
                                    .fd_cache = 8,
                                    .handles = backend::LocalBackend::HandleMode::kFallback,
                                    .max_fallback_entries = 4};
  auto made = backend::LocalBackend::create(cfg);
  ASSERT_TRUE(made.has_value());
  auto backend = std::move(*made);
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2, .ring = "epoll"});
  runtime.start();
  {
    backend::Cred cred{0, 0, {}};
    auto root = run_runtime(runtime, backend->root());
    ASSERT_TRUE(root.has_value());
    for (int i = 0; i < 12; ++i) {
      auto file = run_runtime(runtime, (*root)->lookup(cred, "f" + std::to_string(i)));
      ASSERT_TRUE(file.has_value());
    }
    EXPECT_TRUE(backend->fallback_path_count() <= 4);
    // The most recent handle still resolves; capped-out old ones answer ESTALE.
    auto last = run_runtime(runtime, (*root)->lookup(cred, "f11"));
    ASSERT_TRUE(last.has_value());
    auto resolved = run_runtime(runtime, backend->resolve((*last)->id()));
    EXPECT_TRUE(resolved.has_value());
  }
  runtime.stop_and_join();
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

// Plan doc 10 §2.1: repeated resolve of the same handle hits the O_PATH cache instead
// of paying an offload round-trip + open per request; the object stays fully usable.
TEST(Backend, LocalResolvePathCache) {
  char path_template[] = "/tmp/lightnfs-pc-XXXXXX";
  char* path = mkdtemp(path_template);
  ASSERT_TRUE(path != nullptr);
  std::string file_path = std::string(path) + "/data";
  int fd = open(file_path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
  ASSERT_TRUE(fd >= 0);
  ASSERT_TRUE(write(fd, "payload", 7) == 7);
  close(fd);
  auto made = backend::LocalBackend::create({.path = path, .fsid = 33});
  ASSERT_TRUE(made.has_value());
  auto be = std::move(*made);
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2, .ring = "epoll"});
  runtime.start();
  {
    backend::Cred cred{0, 0, {}};
    auto root = run_runtime(runtime, be->root());
    ASSERT_TRUE(root.has_value());
    auto file = run_runtime(runtime, (*root)->lookup(cred, "data"));
    ASSERT_TRUE(file.has_value());
    backend::ObjId id = (*file)->id();

    auto first = run_runtime(runtime, be->resolve(id));
    ASSERT_TRUE(first.has_value());
    auto before = be->fd_cache_stats();
    auto second = run_runtime(runtime, be->resolve(id));
    ASSERT_TRUE(second.has_value());
    auto after = be->fd_cache_stats();
    EXPECT_TRUE(after.path_hits > before.path_hits);
    EXPECT_TRUE(after.path_entries >= 1);

    // The cache-hit object works end to end (statx via the shared O_PATH fd).
    auto attr = run_runtime(runtime, (*second)->getattr());
    ASSERT_TRUE(attr.has_value());
    EXPECT_EQ(attr->size, 7u);
    // Both objects (shared fd) can be alive and used at once.
    auto attr1 = run_runtime(runtime, (*first)->getattr());
    ASSERT_TRUE(attr1.has_value());
  }
  runtime.stop_and_join();
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

// Per-open data fd (design 05 §5.5, plan doc 10 §5.1): open() hands back a handle
// whose fd serves IO for that open — POSIX open-time permission semantics.  A handle
// without write access falls back to the anonymous fd-cache path (its per-IO checks
// included), which is what a same-owner merge upgrade relies on.
TEST(Backend, LocalOpenStateFdSemantics) {
  char path_template[] = "/tmp/lightnfs-open-XXXXXX";
  char* path = mkdtemp(path_template);
  ASSERT_TRUE(path != nullptr);
  std::string file_path = std::string(path) + "/f";
  int seed_fd = open(file_path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
  ASSERT_TRUE(seed_fd >= 0);
  ASSERT_TRUE(write(seed_fd, "0123456789", 10) == 10);
  close(seed_fd);

  backend::LocalBackend::Config cfg{.path = path,
                                     .fsid = 32,
                                     .fd_cache = 8,
                                     .handles = backend::LocalBackend::HandleMode::kFallback};
  auto made = backend::LocalBackend::create(cfg);
  ASSERT_TRUE(made.has_value());
  auto backend = std::move(*made);
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2, .ring = "epoll"});
  runtime.start();
  backend::Cred owner{geteuid(), getegid(), {}};
  backend::Cred stranger{owner.uid + 12345, owner.gid + 12345, {}};

  auto root = run_runtime(runtime, backend->root());
  ASSERT_TRUE(root.has_value());
  auto file = run_runtime(runtime, (*root)->lookup(owner, "f"));
  ASSERT_TRUE(file.has_value());

  backend::OpenFlags rw;
  rw.set(backend::OpenFlag::kRead).set(backend::OpenFlag::kWrite);
  auto opened = run_runtime(runtime, (*file)->open(owner, rw));
  ASSERT_TRUE(opened.has_value());
  ASSERT_TRUE(*opened != nullptr);

  // Write through the open's own fd, then read it back.
  auto data = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>("ABC"), 3);
  auto wrote = run_runtime(runtime, (*file)->write(backend::OpenCtx{owner, opened->get()},
                                                   0, data, backend::Stability::kUnstable));
  ASSERT_TRUE(wrote.has_value());
  EXPECT_EQ(*wrote, 3u);

  // 0600 file: a stranger's anonymous read is refused, but the same stranger reading
  // through the established open succeeds — permission was settled at OPEN time.
  std::array<std::byte, 16> buf{};
  bool eof = false;
  auto anon = run_runtime(runtime,
                          (*file)->read(backend::OpenCtx{stranger}, 0, buf, eof));
  EXPECT_FALSE(anon.has_value());
  EXPECT_EQ(raw(anon.error()), EACCES);
  auto via_open = run_runtime(
      runtime, (*file)->read(backend::OpenCtx{stranger, opened->get()}, 0, buf, eof));
  ASSERT_TRUE(via_open.has_value());
  EXPECT_EQ(*via_open, 10u);
  EXPECT_EQ(static_cast<char>(buf[0]), 'A');
  EXPECT_EQ(static_cast<char>(buf[3]), '3');

  // A read-only open handle used for WRITE falls back to the anonymous path: the
  // owner passes its checks, the stranger does not.
  backend::OpenFlags ro;
  ro.set(backend::OpenFlag::kRead);
  auto ro_open = run_runtime(runtime, (*file)->open(owner, ro));
  ASSERT_TRUE(ro_open.has_value());
  auto owner_wr = run_runtime(
      runtime, (*file)->write(backend::OpenCtx{owner, ro_open->get()}, 3,
                              data, backend::Stability::kUnstable));
  EXPECT_TRUE(owner_wr.has_value());
  auto stranger_wr = run_runtime(
      runtime, (*file)->write(backend::OpenCtx{stranger, ro_open->get()}, 3,
                              data, backend::Stability::kUnstable));
  EXPECT_FALSE(stranger_wr.has_value());

  runtime.stop_and_join();
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

// ---- plan doc 10 §7.1 additions: kernel-handle mode, identity modes, fd cache ----

namespace {

int count_open_fds() {
  int n = 0;
  for ([[maybe_unused]] auto& e : std::filesystem::directory_iterator("/proc/self/fd")) ++n;
  return n;
}

}  // namespace

TEST(Backend, KernelHandleModeStableAcrossRestart) {
  // Prefer the working directory (usually a real filesystem with exportable handles)
  // over /tmp, which is commonly tmpfs; fall back when it is not writable.
  char cwd_tmpl[] = "lnfs-khandle-XXXXXX";
  char tmp_tmpl[] = "/tmp/lnfs-khandle-XXXXXX";
  char* dir = mkdtemp(cwd_tmpl);
  std::string path = dir ? dir : mkdtemp(tmp_tmpl);
  path = std::filesystem::absolute(path).string();
  std::string file_path = path + "/data";
  int fd = open(file_path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
  ASSERT_TRUE(fd >= 0);
  ASSERT_TRUE(write(fd, "payload", 7) == 7);
  close(fd);

  backend::LocalBackend::Config cfg{.path = path,
                                    .fsid = 33,
                                    .fd_cache = 8,
                                    .handles = backend::LocalBackend::HandleMode::kKernel};
  auto made = backend::LocalBackend::create(cfg);
  if (!made.has_value()) {
    // Forced kernel mode on a filesystem without exportable handles must fail create()
    // rather than degrade silently; nothing more to test on this fs.
    EXPECT_EQ((int)made.error(), EOPNOTSUPP);
    std::printf("  note: kernel handles unavailable on %s (fs support or "
                "CAP_DAC_READ_SEARCH) — create() correctly refused\n",
                path.c_str());
    std::filesystem::remove_all(path);
    return;
  }
  auto backend = std::move(*made);
  EXPECT_TRUE(backend->caps().has(backend::Cap::kStableHandles));
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  backend::Cred cred{static_cast<uint32_t>(getuid()), static_cast<uint32_t>(getgid()), {}};
  auto root = run_runtime(runtime, backend->root());
  ASSERT_TRUE(root.has_value());
  // Kernel-mode ObjIds are tagged with the kernel-handle discriminator byte.
  EXPECT_EQ((int)(*root)->id().view()[0], 1);
  auto file = run_runtime(runtime, (*root)->lookup(cred, "data"));
  ASSERT_TRUE(file.has_value());
  backend::ObjId file_id = (*file)->id();
  backend::ObjId root_id = (*root)->id();
  file = Err(errno_from(ESTALE));
  root = Err(errno_from(ESTALE));
  backend.reset();

  // Restart: kernel handles are derived from the filesystem, so the same object gets
  // the same ObjId — the §1.6 stability property fallback mode cannot give.
  auto restarted = backend::LocalBackend::create(cfg);
  ASSERT_TRUE(restarted.has_value());
  auto root2 = run_runtime(runtime, (*restarted)->root());
  ASSERT_TRUE(root2.has_value());
  EXPECT_TRUE((*root2)->id() == root_id);
  auto file2 = run_runtime(runtime, (*root2)->lookup(cred, "data"));
  ASSERT_TRUE(file2.has_value());
  EXPECT_TRUE((*file2)->id() == file_id);
  // resolve() goes through open_by_handle_at, which needs CAP_DAC_READ_SEARCH; without
  // privilege the kernel answers EPERM (and must NOT be mistaken for ESTALE).
  auto resolved = run_runtime(runtime, (*restarted)->resolve(file_id));
  if (resolved.has_value()) {
    std::array<std::byte, 16> buf{};
    bool eof = false;
    auto n = run_runtime(runtime,
                         (*resolved)->read(backend::OpenCtx{cred}, 0, buf, eof));
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(*n, 7u);
  } else {
    EXPECT_EQ((int)resolved.error(), EPERM);
    std::printf("  note: open_by_handle_at needs CAP_DAC_READ_SEARCH — resolve gave "
                "EPERM as expected for an unprivileged run\n");
  }
  resolved = Err(errno_from(ESTALE));
  file2 = Err(errno_from(ESTALE));
  root2 = Err(errno_from(ESTALE));
  runtime.stop_and_join();
  std::filesystem::remove_all(path);
}

TEST(Backend, IdentityStrictDeniesForeignCred) {
  char tmpl[] = "/tmp/lnfs-strict-XXXXXX";
  std::string path = mkdtemp(tmpl);
  backend::LocalBackend::Config cfg{.path = path,
                                    .fsid = 34,
                                    .handles = backend::LocalBackend::HandleMode::kFallback,
                                    .identity = backend::LocalBackend::Identity::kStrict};
  auto made = backend::LocalBackend::create(cfg);
  ASSERT_TRUE(made.has_value());
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  {
    backend::Cred self{static_cast<uint32_t>(getuid()), static_cast<uint32_t>(getgid()), {}};
    backend::Cred foreign{self.uid + 1000, self.gid + 1000, {}};
    auto root = run_runtime(runtime, (*made)->root());
    ASSERT_TRUE(root.has_value());
    backend::SetAttr attrs;
    attrs.mode = 0600;  // owner-only: the strict check must deny every other uid
    auto created = run_runtime(runtime, (*root)->create(self, "secret", attrs, nullptr));
    ASSERT_TRUE(created.has_value());
    auto w = run_runtime(runtime, created->obj->write(
        backend::OpenCtx{self}, 0, std::span<const std::byte>(
            reinterpret_cast<const std::byte*>("top"), 3), backend::Stability::kUnstable));
    ASSERT_TRUE(w.has_value());

    std::array<std::byte, 8> buf{};
    bool eof = false;
    auto own = run_runtime(runtime,
                           created->obj->read(backend::OpenCtx{self}, 0, buf, eof));
    EXPECT_TRUE(own.has_value());
    auto denied = run_runtime(runtime,
                              created->obj->read(backend::OpenCtx{foreign}, 0, buf, eof));
    ASSERT_TRUE(!denied.has_value());
    EXPECT_EQ((int)denied.error(), EACCES);
    auto wdenied = run_runtime(runtime, created->obj->write(
        backend::OpenCtx{foreign}, 0, std::span<const std::byte>(
            reinterpret_cast<const std::byte*>("x"), 1), backend::Stability::kUnstable));
    ASSERT_TRUE(!wdenied.has_value());
    EXPECT_EQ((int)wdenied.error(), EACCES);
  }
  runtime.stop_and_join();
  std::filesystem::remove_all(path);
}

TEST(Backend, IdentitySetFsuidUnprivilegedIsDocumentedNoop) {
  if (geteuid() == 0) {
    std::printf("  note: running as root — the unprivileged-degraded assertion does not "
                "apply; VM acceptance covers the privileged path\n");
    return;
  }
  char tmpl[] = "/tmp/lnfs-fsuid-XXXXXX";
  std::string path = mkdtemp(tmpl);
  backend::LocalBackend::Config cfg{.path = path,
                                    .fsid = 35,
                                    .handles = backend::LocalBackend::HandleMode::kFallback,
                                    .identity = backend::LocalBackend::Identity::kSetFsuid};
  auto made = backend::LocalBackend::create(cfg);
  ASSERT_TRUE(made.has_value());
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  {
    backend::Cred self{static_cast<uint32_t>(getuid()), static_cast<uint32_t>(getgid()), {}};
    backend::Cred foreign{self.uid + 1000, self.gid + 1000, {}};
    auto root = run_runtime(runtime, (*made)->root());
    ASSERT_TRUE(root.has_value());
    backend::SetAttr attrs;
    attrs.mode = 0600;
    auto created = run_runtime(runtime, (*root)->create(self, "f", attrs, nullptr));
    ASSERT_TRUE(created.has_value());
    // setfsuid mode delegates the check to the kernel; without privilege setfsuid is a
    // no-op (documented in design 06 §6.4), so the foreign write is admitted by the
    // process's own credentials rather than denied in userspace.
    auto w = run_runtime(runtime, created->obj->write(
        backend::OpenCtx{foreign}, 0, std::span<const std::byte>(
            reinterpret_cast<const std::byte*>("k"), 1), backend::Stability::kUnstable));
    EXPECT_TRUE(w.has_value());
  }
  runtime.stop_and_join();
  std::filesystem::remove_all(path);
}

TEST(Backend, FdCacheConcurrentStressFlushAndNoFdLeak) {
  char tmpl[] = "/tmp/lnfs-fdstress-XXXXXX";
  std::string path = mkdtemp(tmpl);
  rt::Runtime runtime({.reactors = 2, .offload_threads = 4});
  runtime.start();
  int fds_baseline = count_open_fds();
  {
    auto made = backend::LocalBackend::create({.path = path, .fsid = 36, .fd_cache = 4});
    ASSERT_TRUE(made.has_value());
    auto& be = **made;
    backend::Cred cred{static_cast<uint32_t>(getuid()), static_cast<uint32_t>(getgid()), {}};
    auto root = run_runtime(runtime, be.root());
    ASSERT_TRUE(root.has_value());

    constexpr int kFiles = 16, kTasks = 8, kIters = 64;
    std::vector<backend::ObjPtr> files;
    for (int i = 0; i < kFiles; ++i) {
      auto created = run_runtime(runtime,
                                 (*root)->create(cred, "f" + std::to_string(i), {}, nullptr));
      ASSERT_TRUE(created.has_value());
      files.push_back(created->obj);
    }

    // 8 tasks × 64 iterations over 16 files on 2 reactors with a 4-entry cache: pins,
    // evictions and (with unlucky interleaving) all-pinned overflow all race here.
    std::atomic<int> failures{0};
    std::mutex mu;
    std::condition_variable cv;
    int done = 0;
    for (int t = 0; t < kTasks; ++t) {
      rt::spawn(
          [](int task, std::vector<backend::ObjPtr>* files, backend::Cred cred,
             std::atomic<int>* failures, std::mutex* mu, std::condition_variable* cv,
             int* done) -> rt::Task<void> {
            std::byte buf[16];
            for (int i = 0; i < kIters; ++i) {
              auto& obj = *(*files)[(task * 7 + i) % kFiles];
              backend::OpenCtx open{cred, nullptr};
              const char msg[] = "stress";
              auto w = co_await obj.write(
                  open, static_cast<uint64_t>(task) * 64,
                  std::span<const std::byte>(reinterpret_cast<const std::byte*>(msg), 6),
                  backend::Stability::kUnstable);
              if (!w) failures->fetch_add(1);
              bool eof = false;
              auto r = co_await obj.read(open, 0, std::span<std::byte>(buf), eof);
              if (!r) failures->fetch_add(1);
            }
            std::lock_guard lock(*mu);
            ++*done;
            cv->notify_one();
          }(t, &files, cred, &failures, &mu, &cv, &done),
          runtime.reactor(t % 2));
    }
    {
      std::unique_lock lock(mu);
      cv.wait(lock, [&] { return done == kTasks; });
    }
    EXPECT_EQ(failures.load(), 0);
    auto st = be.fd_cache_stats();
    // The cap holds unless an eviction pass found every entry pinned (counted).
    EXPECT_TRUE(st.entries <= 4 || st.overflows > 0);
    EXPECT_TRUE(st.hits + st.misses > 0);

    // flush drops every unpinned entry from both caches.
    files.clear();
    root = Err(errno_from(ESTALE));
    size_t flushed = be.flush_fd_cache();
    EXPECT_TRUE(flushed >= 1);
    EXPECT_EQ(be.fd_cache_stats().entries, 0u);
  }
  // Backend gone: every cached/pinned fd must be returned to the process.
  int fds_after = count_open_fds();
  EXPECT_TRUE(fds_after <= fds_baseline + 1);
  runtime.stop_and_join();
  std::filesystem::remove_all(path);
}
