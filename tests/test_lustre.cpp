// Lustre backend (design 06 §6.5) over the in-process kernel-client fake: the design
// 05 §5.9 implementer checklist through the real backend code — FID handle codec
// (P1/P2/ESTALE, restart-stable, rename-proof), namespace ops + readdir enrichment
// carrying FIDs, anonymous / open-state IO on the inherited local paths, HSM-released
// files answering JUKEBOX with a RESTORE kick, stripe-shaped limits, OFD native locks,
// mount-root detection and the config factory (including the real kernel binding
// refusing a non-Lustre directory on this host).

#include "mini_test.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <thread>

#include "backend/fault.hpp"
#include "backend/lustre/lustre.hpp"
#include "llapi_fake.hpp"
#include "reclaim_probe.hpp"
#include "runtime/runtime.hpp"

using namespace lnfs;

namespace {

struct TmpDir {
  std::string path;
  TmpDir() {
    char tmpl[] = "/tmp/lnfs-lustre-XXXXXX";
    path = mkdtemp(tmpl);
  }
  ~TmpDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

template <class T>
T run(rt::Runtime& runtime, rt::Task<T> task) {
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
          cv->notify_one();
        }
      }(std::move(task), &mu, &cv, &result),
      runtime.reactor(0));
  std::unique_lock lock(mu);
  cv.wait(lock, [&] { return result.has_value(); });
  return std::move(*result);
}

struct Options {
  bool hsm = true;
  bool locks = true;
  std::string mount;   // explicit mount root
  std::string reuse;   // serve this directory instead of a fresh one (no fake reset)
  uint32_t stripe = 0;
  Options& no_hsm() { hsm = false; return *this; }
  Options& no_locks() { locks = false; return *this; }
  Options& at(std::string m) { mount = std::move(m); return *this; }
  Options& reusing(std::string dir) { reuse = std::move(dir); return *this; }
  Options& striped(uint32_t bytes) { stripe = bytes; return *this; }
};

// A started fake-backed Lustre export.
struct Mount {
  std::optional<TmpDir> dir;
  std::string path;
  rt::Runtime runtime{{.reactors = 1, .offload_threads = 4}};
  std::unique_ptr<backend::LustreBackend> be;
  backend::Cred root_cred{0, 0, {}};

  explicit Mount(Options o = {}) {
    if (o.reuse.empty()) {
      dir.emplace();
      path = dir->path;
      testing::FakeLlapi::reset();
    } else {
      path = o.reuse;
    }
    if (o.stripe) testing::FakeLlapi::set_stripe_size(o.stripe);
    backend::fault::clear();
    runtime.start();
    backend::LustreBackend::Config cfg;
    cfg.path = path;
    cfg.fsid = 11;
    cfg.mount = o.mount;
    cfg.hsm = o.hsm;
    cfg.native_locks = o.locks;
    auto made = backend::LustreBackend::create(cfg, testing::FakeLlapi::ops());
    ASSERT_TRUE(made.has_value());
    be = std::move(*made);
    auto started = run(runtime, be->start());
    ASSERT_TRUE(started.has_value());
  }
  ~Mount() {
    if (be) (void)run(runtime, be->stop());
    be.reset();
    runtime.stop_and_join();
  }
  backend::ObjPtr root() {
    auto r = run(runtime, be->root());
    EXPECT_TRUE(r.has_value());
    return r ? *r : nullptr;
  }
  std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
  }
  backend::ObjPtr create_file(std::string_view name, std::string_view content) {
    auto root = this->root();
    auto f = run(runtime, root->create(root_cred, name, backend::SetAttr{}, nullptr));
    EXPECT_TRUE(f.has_value());
    if (!f) return nullptr;
    if (!content.empty()) {
      auto n = run(runtime, f->obj->write({root_cred}, 0, bytes(content),
                                          backend::Stability::kFileSync));
      EXPECT_TRUE(n.has_value());
    }
    return f->obj;
  }
  std::string read_all(backend::Object& f, size_t cap = 4096) {
    std::vector<std::byte> buf(cap);
    bool eof = false;
    auto n = run(runtime, f.read({root_cred}, 0, std::span(buf), eof));
    EXPECT_TRUE(n.has_value());
    if (!n) return "<read failed>";
    return std::string(reinterpret_cast<const char*>(buf.data()), *n);
  }
};

}  // namespace

TEST(Lustre, CapsHandlesAndResolve) {
  Mount m;
  auto caps = m.be->caps();
  EXPECT_TRUE(caps.has(backend::Cap::kStableHandles));
  EXPECT_TRUE(caps.has(backend::Cap::kJukebox));
  EXPECT_TRUE(caps.has(backend::Cap::kByteLocks));
  EXPECT_TRUE(caps.has(backend::Cap::kSymlink));
  EXPECT_TRUE(caps.has(backend::Cap::kHardlink));
  EXPECT_FALSE(caps.has(backend::Cap::kNativeAccess));
  EXPECT_FALSE(caps.has(backend::Cap::kNativeChange));  // change is ctime-synthesized
  EXPECT_TRUE(m.be->native_locks().has_value());
  EXPECT_EQ(m.be->fsid(), 11u);
  EXPECT_TRUE(m.be->stable_handles());

  auto root = m.root();
  EXPECT_EQ(root->id().len, 17);  // tag + lu_fid
  EXPECT_EQ(static_cast<int>(root->id().bytes[0]), 4);
  EXPECT_EQ(root->type(), backend::FType::kDir);
  auto fid = backend::LustreBackend::fid_from_oid(root->id());
  ASSERT_TRUE(fid.has_value());
  EXPECT_TRUE(*fid == testing::FakeLlapi::fid_of_path(m.path));
  EXPECT_TRUE(backend::LustreBackend::oid_from_fid(*fid) == root->id());
  EXPECT_TRUE(backend::llapi::fid_to_string(*fid).starts_with("0x200000401:0x"));

  auto again = run(m.runtime, m.be->resolve(root->id()));
  ASSERT_TRUE(again.has_value());
  EXPECT_TRUE((*again)->id() == root->id());
  auto third = run(m.runtime, m.be->resolve(root->id()));
  ASSERT_TRUE(third.has_value());
  EXPECT_TRUE(m.be->fd_cache_stats().path_hits >= 1);  // O_PATH resolve cache hit

  // Malformed / foreign handle bytes → ESTALE, never a crash.
  std::array<std::byte, 17> gluster_like{};
  gluster_like[0] = std::byte{3};
  auto g = run(m.runtime, m.be->resolve(*backend::ObjId::from(gluster_like)));
  EXPECT_FALSE(g.has_value());
  EXPECT_EQ(raw(g.error()), ESTALE);
  std::array<std::byte, 21> local_like{};
  local_like[0] = std::byte{2};
  auto l = run(m.runtime, m.be->resolve(*backend::ObjId::from(local_like)));
  EXPECT_FALSE(l.has_value());
  EXPECT_EQ(raw(l.error()), ESTALE);
  std::array<std::byte, 17> zero_fid{};
  zero_fid[0] = std::byte{4};
  auto z = run(m.runtime, m.be->resolve(*backend::ObjId::from(zero_fid)));
  EXPECT_FALSE(z.has_value());
  EXPECT_EQ(raw(z.error()), ESTALE);
  std::array<std::byte, 16> short_one{};
  short_one[0] = std::byte{4};
  EXPECT_FALSE(backend::LustreBackend::fid_from_oid(*backend::ObjId::from(short_one)).has_value());
  backend::llapi::Fid unknown{0x200000401ull, 0xfffffff0u, 7};  // never pinned
  auto u = run(m.runtime, m.be->resolve(backend::LustreBackend::oid_from_fid(unknown)));
  EXPECT_FALSE(u.has_value());
  EXPECT_EQ(raw(u.error()), ESTALE);
}

TEST(Lustre, FidHandlesSurviveRenameAndRestart) {
  TmpDir dir;  // outlives both backend instances
  testing::FakeLlapi::reset();
  backend::ObjId file_oid, dir_oid, link_oid;
  uint64_t fileid = 0;
  {
    Mount m(Options{}.reusing(dir.path));
    auto root = m.root();
    auto f = m.create_file("a.txt", "alpha");
    ASSERT_TRUE(f != nullptr);
    file_oid = f->id();
    auto d = run(m.runtime, root->mkdir(m.root_cred, "sub", backend::SetAttr{}));
    ASSERT_TRUE(d.has_value());
    dir_oid = d->obj->id();
    auto s = run(m.runtime, root->symlink(m.root_cred, "lnk", "a.txt", backend::SetAttr{}));
    ASSERT_TRUE(s.has_value());
    link_oid = s->obj->id();
    auto attr = run(m.runtime, f->getattr());
    ASSERT_TRUE(attr.has_value());
    fileid = attr->fileid;

    // A rename moves the object; the FID (and therefore the filehandle) is unchanged
    // and keeps resolving — no path bookkeeping involved.
    ASSERT_TRUE(run(m.runtime, root->rename(m.root_cred, "a.txt", *d->obj, "b.txt")).has_value());
    auto moved = run(m.runtime, m.be->resolve(file_oid));
    ASSERT_TRUE(moved.has_value());
    EXPECT_STREQ(m.read_all(**moved), "alpha");
    auto via_lookup = run(m.runtime, d->obj->lookup(m.root_cred, "b.txt"));
    ASSERT_TRUE(via_lookup.has_value());
    EXPECT_TRUE((*via_lookup)->id() == file_oid);
    // symlink objects resolve by FID too (O_PATH|O_NOFOLLOW semantics kept)
    auto lnk = run(m.runtime, m.be->resolve(link_oid));
    ASSERT_TRUE(lnk.has_value());
    EXPECT_EQ((*lnk)->type(), backend::FType::kLnk);
    auto target = run(m.runtime, (*lnk)->readlink());
    ASSERT_TRUE(target.has_value());
    EXPECT_STREQ(*target, "a.txt");
    // readdir enrichment carries FIDs and attrs
    auto page = run(m.runtime, d->obj->readdir(m.root_cred, 0, 16));
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(page->ents.size(), 1u);
    EXPECT_TRUE(page->eof);
    ASSERT_TRUE(page->ents[0].oid.has_value());
    EXPECT_TRUE(*page->ents[0].oid == file_oid);
    ASSERT_TRUE(page->ents[0].attr.has_value());
    EXPECT_EQ(page->ents[0].attr->size, 5u);
  }
  {
    // A fresh backend on the same tree (restart): the old handles are still valid.
    Mount m(Options{}.reusing(dir.path));
    auto f = run(m.runtime, m.be->resolve(file_oid));
    ASSERT_TRUE(f.has_value());
    auto attr = run(m.runtime, (*f)->getattr());
    ASSERT_TRUE(attr.has_value());
    EXPECT_EQ(attr->fileid, fileid);
    EXPECT_STREQ(m.read_all(**f), "alpha");
    auto d = run(m.runtime, m.be->resolve(dir_oid));
    ASSERT_TRUE(d.has_value());
    // Unlink → the FID no longer resolves (ESTALE), and a recreated name gets a new
    // FID (P2).  The O_PATH resolve cache may keep a deleted object resolvable until
    // its entry is dropped (documented bounded staleness, plan doc 10 §2.1): release
    // our reference and flush before asserting.
    ASSERT_TRUE(run(m.runtime, (*d)->unlink(m.root_cred, "b.txt")).has_value());
    f->reset();
    (void)m.be->flush_fd_cache();
    auto gone = run(m.runtime, m.be->resolve(file_oid));
    EXPECT_FALSE(gone.has_value());
    EXPECT_EQ(raw(gone.error()), ESTALE);
    auto again = run(m.runtime, (*d)->create(m.root_cred, "b.txt", backend::SetAttr{}, nullptr));
    ASSERT_TRUE(again.has_value());
    EXPECT_FALSE(again->obj->id() == file_oid);
  }
}

TEST(Lustre, AnonymousAndOpenStateIo) {
  Mount m;
  auto f = m.create_file("io", "");
  ASSERT_TRUE(f != nullptr);
  backend::OpenFlags rw;
  rw.set(backend::OpenFlag::kRead).set(backend::OpenFlag::kWrite);
  auto open = run(m.runtime, f->open(m.root_cred, rw));
  ASSERT_TRUE(open.has_value());
  backend::OpenCtx octx{m.root_cred, open->get()};
  auto n = run(m.runtime,
               f->write(octx, 0, m.bytes("hello lustre"), backend::Stability::kUnstable));
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(*n, 12u);
  ASSERT_TRUE(run(m.runtime, f->commit(octx, 0, 0)).has_value());
  EXPECT_STREQ(m.read_all(*f), "hello lustre");
  // setattr(mode) goes through the FID-opened descriptor
  backend::SetAttr chmod;
  chmod.mode = 0600;
  auto changed = run(m.runtime, f->setattr(m.root_cred, chmod));
  ASSERT_TRUE(changed.has_value());
  EXPECT_EQ(changed->mode, 0600u);
  // truncate through setattr and seek
  backend::SetAttr trunc;
  trunc.size = 5;
  auto after = run(m.runtime, f->setattr(m.root_cred, trunc));
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->size, 5u);
  EXPECT_STREQ(m.read_all(*f), "hello");
  auto hole = run(m.runtime, f->seek({m.root_cred}, 0, backend::SeekWhat::kHole));
  if (hole) EXPECT_EQ(*hole, 5u);
  // the data fd cache is the local one, keyed by the FID handle
  auto stats = m.be->fd_cache_stats();
  EXPECT_TRUE(stats.entries >= 1);
  EXPECT_TRUE(m.be->stats().hsm_checks >= 1);  // every regular-file data open is gated
}

TEST(Lustre, HsmReleasedFileIsJukeboxUntilRestored) {
  Mount m;
  auto f = m.create_file("cold", "archived bytes");
  ASSERT_TRUE(f != nullptr);
  auto fid = *backend::LustreBackend::fid_from_oid(f->id());
  ASSERT_TRUE(m.be->flush_fd_cache() >= 1);  // forget the descriptor the write cached
  testing::FakeLlapi::set_hsm_states(
      fid, backend::llapi::kHsExists | backend::llapi::kHsArchived | backend::llapi::kHsReleased);

  std::vector<std::byte> buf(64);
  bool eof = false;
  auto first = run(m.runtime, f->read({m.root_cred}, 0, std::span(buf), eof));
  EXPECT_FALSE(first.has_value());
  EXPECT_EQ(first.error(), Errno::kJukebox);
  auto s = m.be->stats();
  EXPECT_EQ(s.jukebox, 1u);
  EXPECT_EQ(s.hsm_restores, 1u);
  auto requests = testing::FakeLlapi::restore_requests();
  ASSERT_TRUE(requests.size() == 1);
  EXPECT_TRUE(requests[0] == fid);
  // Still released, restore in progress: JUKEBOX again but no second request.
  auto second = run(m.runtime, f->read({m.root_cred}, 0, std::span(buf), eof));
  EXPECT_FALSE(second.has_value());
  EXPECT_EQ(second.error(), Errno::kJukebox);
  EXPECT_EQ(m.be->stats().hsm_restores, 1u);
  EXPECT_EQ(m.be->stats().jukebox, 2u);
  // A v4 OPEN on the released file is DELAY too (not the EOPNOTSUPP degradation).
  backend::OpenFlags ro;
  ro.set(backend::OpenFlag::kRead);
  auto open = run(m.runtime, f->open(m.root_cred, ro));
  EXPECT_FALSE(open.has_value());
  EXPECT_EQ(open.error(), Errno::kJukebox);
  // Writes and truncates are gated the same way (they would block on the restore).
  auto w = run(m.runtime, f->write({m.root_cred}, 0, m.bytes("x"), backend::Stability::kUnstable));
  EXPECT_FALSE(w.has_value());
  EXPECT_EQ(w.error(), Errno::kJukebox);
  // Directories are never HSM-gated.
  auto page = run(m.runtime, m.root()->readdir(m.root_cred, 0, 8));
  EXPECT_TRUE(page.has_value());

  // The coordinator finishes: the next open goes through and gets cached as usual.
  testing::FakeLlapi::set_hsm_states(fid, backend::llapi::kHsExists | backend::llapi::kHsArchived);
  EXPECT_STREQ(m.read_all(*f), "archived bytes");
  EXPECT_EQ(m.be->stats().jukebox, 4u);
  EXPECT_EQ(m.be->stats().hsm_restores, 1u);

  // A fast coordinator (auto restore) still answers JUKEBOX once: the client retry
  // is what picks up the restored data.
  ASSERT_TRUE(m.be->flush_fd_cache() >= 1);
  testing::FakeLlapi::set_auto_restore(true);
  testing::FakeLlapi::set_hsm_states(
      fid, backend::llapi::kHsExists | backend::llapi::kHsArchived | backend::llapi::kHsReleased);
  auto again = run(m.runtime, f->read({m.root_cred}, 0, std::span(buf), eof));
  EXPECT_FALSE(again.has_value());
  EXPECT_EQ(again.error(), Errno::kJukebox);
  EXPECT_EQ(testing::FakeLlapi::hsm_states(fid) & backend::llapi::kHsReleased, 0u);
  EXPECT_STREQ(m.read_all(*f), "archived bytes");

  // A client without HSM (ENOTTY) is simply not gated.
  ASSERT_TRUE(m.be->flush_fd_cache() >= 1);
  testing::FakeLlapi::set_hsm_supported(false);
  testing::FakeLlapi::set_hsm_states(fid, backend::llapi::kHsReleased);
  EXPECT_STREQ(m.read_all(*f), "archived bytes");
}

TEST(Lustre, HsmDisabledServesReleasedFilesInline) {
  Mount m(Options{}.no_hsm());
  EXPECT_FALSE(m.be->caps().has(backend::Cap::kJukebox));
  auto f = m.create_file("cold", "inline restore");
  ASSERT_TRUE(f != nullptr);
  auto fid = *backend::LustreBackend::fid_from_oid(f->id());
  (void)m.be->flush_fd_cache();
  testing::FakeLlapi::set_hsm_states(fid, backend::llapi::kHsReleased);
  EXPECT_STREQ(m.read_all(*f), "inline restore");  // the kernel would restore inline
  EXPECT_EQ(m.be->stats().jukebox, 0u);
  EXPECT_EQ(m.be->stats().hsm_checks, 0u);
  EXPECT_TRUE(testing::FakeLlapi::restore_requests().empty());
}

TEST(Lustre, NativeByteRangeLocks) {
  Mount m;
  auto root = m.root();
  auto f = run(m.runtime, root->create(m.root_cred, "lk", backend::SetAttr{}, nullptr));
  ASSERT_TRUE(f.has_value());
  auto& mgr = m.be->native_locks()->get();
  backend::LockOwnerId a, b;
  a.len = 4;
  std::memcpy(a.bytes.data(), "own1", 4);
  b.len = 4;
  std::memcpy(b.bytes.data(), "own2", 4);
  auto& obj = *f->obj;

  ASSERT_TRUE(run(m.runtime, mgr.lock(obj, a, {0, 10}, true, false)).has_value());
  EXPECT_EQ(m.be->stats().lock_fds, 1u);
  auto conflict = run(m.runtime, mgr.lock(obj, b, {5, 10}, true, false));
  EXPECT_FALSE(conflict.has_value());
  EXPECT_EQ(raw(conflict.error()), EAGAIN);
  auto probe = run(m.runtime, mgr.test(obj, {5, 1}, false));
  ASSERT_TRUE(probe.has_value());
  ASSERT_TRUE(probe->has_value());
  EXPECT_TRUE((*probe)->exclusive);
  EXPECT_EQ((*probe)->range.offset, 0u);
  EXPECT_EQ((*probe)->range.length, 10u);
  EXPECT_EQ((*probe)->owner.len, 0);  // holder identity unknown (OFD)
  auto clear = run(m.runtime, mgr.test(obj, {10, UINT64_MAX}, true));
  ASSERT_TRUE(clear.has_value());
  EXPECT_FALSE(clear->has_value());
  // shared locks coexist; same owner re-locks freely
  ASSERT_TRUE(run(m.runtime, mgr.lock(obj, b, {20, 5}, false, false)).has_value());
  ASSERT_TRUE(run(m.runtime, mgr.lock(obj, a, {20, 5}, false, false)).has_value());
  ASSERT_TRUE(run(m.runtime, mgr.lock(obj, a, {0, 10}, true, false)).has_value());
  // unlock part, the rest still blocks
  ASSERT_TRUE(run(m.runtime, mgr.unlock(obj, a, {0, 5})).has_value());
  ASSERT_TRUE(run(m.runtime, mgr.lock(obj, b, {0, 5}, true, false)).has_value());
  EXPECT_FALSE(run(m.runtime, mgr.lock(obj, b, {5, 5}, true, false)).has_value());
  // release closes the owner's descriptor and frees everything it held
  ASSERT_TRUE(run(m.runtime, mgr.release(obj, a)).has_value());
  EXPECT_EQ(m.be->stats().lock_fds, 1u);
  ASSERT_TRUE(run(m.runtime, mgr.lock(obj, b, {5, 5}, true, false)).has_value());
  ASSERT_TRUE(run(m.runtime, mgr.release(obj, b)).has_value());
  EXPECT_EQ(m.be->stats().lock_fds, 0u);
  // unlocking what was never held is fine; to-EOF ranges are accepted
  EXPECT_TRUE(run(m.runtime, mgr.unlock(obj, a, {0, UINT64_MAX})).has_value());
  EXPECT_TRUE(run(m.runtime, mgr.lock(obj, a, {100, UINT64_MAX}, true, false)).has_value());
  auto eof_conf = run(m.runtime, mgr.test(obj, {1000, 1}, false));
  ASSERT_TRUE(eof_conf.has_value());
  ASSERT_TRUE(eof_conf->has_value());
  EXPECT_EQ((*eof_conf)->range.length, UINT64_MAX);
  // locks on a directory are meaningless
  auto on_dir = run(m.runtime, mgr.lock(*root, a, {0, 1}, true, false));
  EXPECT_FALSE(on_dir.has_value());
  EXPECT_EQ(raw(on_dir.error()), EINVAL);
  // a lock on a released file is DELAY like any other data open
  auto fid = *backend::LustreBackend::fid_from_oid(obj.id());
  auto cold = run(m.runtime, root->create(m.root_cred, "cold", backend::SetAttr{}, nullptr));
  ASSERT_TRUE(cold.has_value());
  auto cold_fid = *backend::LustreBackend::fid_from_oid(cold->obj->id());
  (void)fid;
  testing::FakeLlapi::set_hsm_states(cold_fid, backend::llapi::kHsReleased);
  auto delayed = run(m.runtime, mgr.lock(*cold->obj, b, {0, 1}, true, false));
  EXPECT_FALSE(delayed.has_value());
  EXPECT_EQ(delayed.error(), Errno::kJukebox);
  // stop() closes every lock descriptor
  ASSERT_TRUE(run(m.runtime, m.be->stop()).has_value());
  EXPECT_EQ(m.be->stats().lock_fds, 0u);
}

TEST(Lustre, NativeLocksOff) {
  Mount m(Options{}.no_locks());
  EXPECT_FALSE(m.be->caps().has(backend::Cap::kByteLocks));
  EXPECT_FALSE(m.be->native_locks().has_value());
  EXPECT_EQ(m.be->stats().lock_fds, 0u);
}

TEST(Lustre, StripeSizeShapesTransferHints) {
  {
    Mount m;  // no layout on the root: defaults stay
    EXPECT_EQ(m.be->limits().pref_read, 1u << 20);
    EXPECT_EQ(m.be->limits().pref_write, 1u << 20);
  }
  {
    Mount m(Options{}.striped(64u << 10));
    EXPECT_EQ(m.be->limits().pref_read, 64u << 10);
    EXPECT_EQ(m.be->limits().pref_write, 64u << 10);
  }
  {
    Mount m(Options{}.striped(4u << 20));  // wider than max_read: clamped
    EXPECT_EQ(m.be->limits().pref_read, m.be->limits().max_read);
    EXPECT_EQ(m.be->limits().pref_write, m.be->limits().max_write);
  }
  // lov_user_md parsing on synthetic blobs
  unsigned char v1[32] = {};
  uint32_t magic = 0x0BD10BD0, stripe = 2u << 20;
  std::memcpy(v1, &magic, 4);
  std::memcpy(v1 + 24, &stripe, 4);
  auto parsed = backend::llapi::stripe_size_from_lov(v1, sizeof v1);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, 2u << 20);
  unsigned char comp[96] = {};
  uint32_t comp_magic = 0x0BD60BD0, v3 = 0x0BD30BD0, s3 = 512u << 10;
  std::memcpy(comp, &comp_magic, 4);
  std::memcpy(comp + 48, &v3, 4);
  std::memcpy(comp + 48 + 24, &s3, 4);
  auto pfl = backend::llapi::stripe_size_from_lov(comp, sizeof comp);
  ASSERT_TRUE(pfl.has_value());
  EXPECT_EQ(*pfl, 512u << 10);
  unsigned char foreign[32] = {0xD0, 0x0B, 0xD7, 0x0B};
  EXPECT_FALSE(backend::llapi::stripe_size_from_lov(foreign, sizeof foreign).has_value());
  EXPECT_FALSE(backend::llapi::stripe_size_from_lov(v1, 8).has_value());
}

TEST(Lustre, MountRootDetectionAndRejections) {
  TmpDir dir;
  std::filesystem::create_directories(dir.path + "/projects/a");
  struct stat export_st {};
  ASSERT_TRUE(::stat((dir.path + "/projects/a").c_str(), &export_st) == 0);
  {
    // Auto-detected mount root: an ancestor on the same device, containing the export.
    Mount m(Options{}.reusing(dir.path + "/projects/a"));
    testing::FakeLlapi::reset();
    const auto& root = m.be->mount_path();
    EXPECT_TRUE((dir.path + "/projects/a").starts_with(root));
    struct stat st {};
    ASSERT_TRUE(::stat(root.c_str(), &st) == 0);
    EXPECT_EQ(st.st_dev, export_st.st_dev);
    EXPECT_STREQ(m.be->root_path(), dir.path + "/projects/a");
  }
  {
    // Explicit mount root wins.
    Mount m(Options{}.at(dir.path).reusing(dir.path + "/projects/a"));
    EXPECT_STREQ(m.be->mount_path(), dir.path);
  }
  // A mount root on another device is rejected (the export is not inside it).
  backend::LustreBackend::Config cfg;
  cfg.path = dir.path + "/projects/a";
  cfg.fsid = 3;
  cfg.mount = "/proc";
  auto xdev = backend::LustreBackend::create(cfg, testing::FakeLlapi::ops());
  EXPECT_FALSE(xdev.has_value());
  EXPECT_EQ(raw(xdev.error()), EXDEV);
  // Not a Lustre filesystem → EOPNOTSUPP (fake switch, and the real kernel binding on
  // this host's tmp directory).
  cfg.mount.clear();
  testing::FakeLlapi::set_lustre(false);
  auto not_lustre = backend::LustreBackend::create(cfg, testing::FakeLlapi::ops());
  EXPECT_FALSE(not_lustre.has_value());
  EXPECT_EQ(raw(not_lustre.error()), EOPNOTSUPP);
  testing::FakeLlapi::set_lustre(true);
  auto real = backend::LustreBackend::create(cfg);
  EXPECT_FALSE(real.has_value());
  EXPECT_EQ(raw(real.error()), EOPNOTSUPP);
  // Missing export directory / zero fsid.
  backend::LustreBackend::Config missing = cfg;
  missing.path = dir.path + "/nope";
  EXPECT_FALSE(backend::LustreBackend::create(missing, testing::FakeLlapi::ops()).has_value());
  backend::LustreBackend::Config zero = cfg;
  zero.fsid = 0;
  EXPECT_FALSE(backend::LustreBackend::create(zero, testing::FakeLlapi::ops()).has_value());
}

TEST(Lustre, ConfigFactory) {
  backend::register_builtin_backends();
  const auto* factory = backend::find_backend("lustre");
  ASSERT_TRUE(factory != nullptr);
  EXPECT_FALSE(factory->virtual_path);  // path is a real directory inside the mount
  auto names = backend::registered_backends();
  EXPECT_TRUE(std::set<std::string>(names.begin(), names.end()).contains("lustre"));

  TmpDir dir;
  backend::BackendConfig cfg;
  cfg.path = dir.path;
  cfg.fsid = 5;
  cfg.values["mount"] = dir.path;
  cfg.values["fd_cache"] = "128";
  cfg.values["identity"] = "strict";
  cfg.values["readdir_enrich"] = "false";
  cfg.values["hsm"] = "false";
  cfg.values["native_locks"] = "false";
  // The factory binds the real kernel client: on this host that is not Lustre.
  EXPECT_TRUE(factory->make(cfg) == nullptr);
  // Value validation happens before the mount is touched.
  backend::BackendConfig bad = cfg;
  bad.values["hsm"] = "yes";
  EXPECT_TRUE(factory->make(bad) == nullptr);
  backend::BackendConfig bad_id = cfg;
  bad_id.values["identity"] = "root";
  EXPECT_TRUE(factory->make(bad_id) == nullptr);
  backend::BackendConfig bad_cache = cfg;
  bad_cache.values["fd_cache"] = "0";
  EXPECT_TRUE(factory->make(bad_cache) == nullptr);
  backend::BackendConfig unknown = cfg;
  unknown.values["volume"] = "x";
  EXPECT_TRUE(factory->make(unknown) == nullptr);

  // The same knobs through the typed config, on the fake.
  testing::FakeLlapi::reset();
  backend::LustreBackend::Config typed;
  typed.path = dir.path;
  typed.fsid = 5;
  typed.fd_cache = 128;
  typed.identity = backend::LocalBackend::Identity::kStrict;
  typed.enrich_readdir = false;
  typed.hsm = false;
  typed.native_locks = false;
  auto made = backend::LustreBackend::create(typed, testing::FakeLlapi::ops());
  ASSERT_TRUE(made.has_value());
  EXPECT_EQ((*made)->config().fd_cache, 128u);
  EXPECT_TRUE((*made)->config().identity == backend::LocalBackend::Identity::kStrict);
  EXPECT_FALSE((*made)->config().enrich_readdir);
  EXPECT_FALSE((*made)->caps().has(backend::Cap::kJukebox));
  EXPECT_FALSE((*made)->caps().has(backend::Cap::kByteLocks));
  EXPECT_TRUE((*made)->caps().has(backend::Cap::kStableHandles));
  EXPECT_FALSE((*made)->lustre_config().hsm);
  made->reset();
}

// plan 10 E2: the failed gateway's OFD lock still sits on the file when the
// taking-over gateway's client reclaims inside grace, so the push (a real
// F_OFD_SETLK from the backend's own descriptor) is refused and the state layer
// answers DELAY (plan 10 B2); the lock is dropped after a delay — the client eviction
// Lustre does on obd_timeout — and the retry wins, all in grace.  A competing OFD lock
// on the backing file from a separate descriptor stands in for the dead gateway.
TEST(Lustre, ReclaimLockDelayUntilClientEviction) {
  Mount m;
  auto file = m.create_file("lk", "seed");
  ASSERT_TRUE(file != nullptr);
  backend::ObjId oid = file->id();
  std::string backing = m.path + "/lk";

  // The dead gateway's residue: a real OFD write lock held by another descriptor.
  int stale_fd = ::open(backing.c_str(), O_RDWR);
  ASSERT_TRUE(stale_fd >= 0);
  struct flock fl {};
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 100;
  ASSERT_TRUE(::fcntl(stale_fd, F_OFD_SETLK, &fl) == 0);

  auto* mgr = &m.be->native_locks()->get();
  TmpDir state_dir;
  test::ReclaimProbe probe(
      m.runtime, state_dir.path, /*fsid=*/11, oid,
      [mgr](uint32_t) -> backend::LockMgr* { return mgr; },
      [be = m.be.get()](uint32_t, const backend::ObjId& o)
          -> rt::Task<Result<backend::ObjPtr>> { co_return co_await be->resolve(o); });
  ASSERT_TRUE(probe.in_grace());
  EXPECT_EQ(probe.open_reclaim(), 0u);

  // Refused while the OFD lock is held: DELAY, no state minted.
  EXPECT_EQ(probe.lock_reclaim(), test::ReclaimProbe::delay());
  EXPECT_TRUE(probe.reclaim_delays() >= 1u);
  EXPECT_EQ(probe.lock_states(), 0u);

  // The MDS evicts the dead client after ~150 ms: its OFD lock is dropped (fd closed).
  std::thread evict([stale_fd] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ::close(stale_fd);  // closing the descriptor releases its OFD locks
  });
  auto out = probe.lock_until_settled(/*attempts=*/100, std::chrono::milliseconds(20));
  evict.join();
  EXPECT_EQ(out.status, 0u);
  EXPECT_TRUE(out.delays >= 1u);
  EXPECT_EQ(probe.lock_states(), 1u);
  EXPECT_TRUE(probe.in_grace());
}
