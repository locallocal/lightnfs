// Fuzz target for LocalBackend handle-content parsing (plan doc 10 §7.2): ObjId bytes
// come straight out of client filehandles, and LocalBackend::open_oid is the parse
// boundary (kernel-handle layout checks, fallback layout size check, reverse-map lookup,
// open + re-verify).  Raw input and mutations of a genuinely valid fallback ObjId both
// go through ObjId::from + open_oid.
//
// Needs a real directory (there is no in-memory LocalBackend): a private mkdtemp tree
// under $TMPDIR, populated once and removed at exit.  A small Runtime resolves one file
// so the fallback reverse map has a live entry to hit.

#include <fcntl.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

#include "backend/local.hpp"
#include "runtime/runtime.hpp"
#include "util/log.hpp"

using namespace lnfs;

namespace {

std::string g_dir;
void cleanup_dir() {
  std::error_code ec;
  if (!g_dir.empty()) std::filesystem::remove_all(g_dir, ec);
}

template <class T>
T run_blocking(rt::Runtime& runtime, rt::Task<T> task) {
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

struct Env {
  std::unique_ptr<rt::Runtime> runtime;
  std::unique_ptr<backend::LocalBackend> be;
  backend::ObjId valid{};  // a live fallback ObjId (reverse map populated)

  Env() {
    lnfs::set_log_level(lnfs::LogLevel::kError);
    const char* base = std::getenv("TMPDIR");
    std::string tmpl = std::string(base && *base ? base : "/tmp") + "/lnfs-fuzz-oid-XXXXXX";
    std::vector<char> buf(tmpl.c_str(), tmpl.c_str() + tmpl.size() + 1);
    if (const char* got = mkdtemp(buf.data())) g_dir = got;
    std::atexit(cleanup_dir);
    int fd = ::open((g_dir + "/seed").c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    if (fd >= 0) {
      (void)!::write(fd, "seed", 4);
      ::close(fd);
    }
    runtime = std::make_unique<rt::Runtime>(
        rt::Runtime::Config{.reactors = 1, .offload_threads = 1});
    runtime->start();
    auto made = backend::LocalBackend::create(
        {.path = g_dir, .fsid = 1, .handles = backend::LocalBackend::HandleMode::kAuto});
    if (!made) std::abort();
    be = std::move(*made);
    auto root = run_blocking(*runtime, be->root());
    if (root) {
      auto file = run_blocking(*runtime, (*root)->lookup(backend::Cred{0, 0, {}}, "seed"));
      if (file) valid = (*file)->id();
    }
  }
};

Env& env() {
  static Env e;
  return e;
}

void probe(backend::LocalBackend& be, std::span<const std::byte> bytes) {
  auto oid = backend::ObjId::from(bytes);
  if (!oid) return;
  auto fd = be.open_oid(*oid, O_PATH | O_NOFOLLOW);
  if (fd) ::close(*fd);
}

}  // namespace

extern "C" void lnfs_fuzz_entry(const uint8_t* data, size_t size) {
  auto& e = env();
  probe(*e.be, std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size));
  // Mutations of a live ObjId reach the deeper stages (map hit, open, re-verify).
  if (size >= 2 && e.valid.len > 0) {
    auto mutated = e.valid;
    mutated.bytes[data[0] % mutated.len] ^= std::byte(data[1]);
    probe(*e.be, mutated.view());
  }
}

#ifndef LNFS_FUZZ_REGRESS
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lnfs_fuzz_entry(data, size);
  return 0;
}
#endif
