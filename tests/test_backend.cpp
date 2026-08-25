#include "mini_test.hpp"

#include <arpa/inet.h>

#include <array>
#include <condition_variable>
#include <filesystem>
#include <fcntl.h>
#include <mutex>
#include <unordered_set>
#include <unistd.h>

#include "backend/memory.hpp"
#include "backend/local.hpp"
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
