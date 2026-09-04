// GlusterFS backend (plan doc 10 §5.3) over the in-process libgfapi fake: the design
// 05 §5.9 implementer checklist, exercised end to end through the real backend code
// — handle codec (P1/P2/ESTALE), namespace ops, EXCLUSIVE replay, readdir cookies
// with enrichment, anonymous + open-state IO, sticky commit poison, v4.2 ops, native
// access under the caller's identity, jukebox mapping, native byte-range locks and the
// config factory.

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

#include "backend/fault.hpp"
#include "backend/gluster/gluster.hpp"
#include "gfapi_fake.hpp"
#include "runtime/runtime.hpp"

using namespace lnfs;

namespace {

struct TmpDir {
  std::string path;
  TmpDir() {
    char tmpl[] = "/tmp/lnfs-gluster-XXXXXX";
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

// A started fake-backed volume export.
struct Volume {
  TmpDir dir;
  rt::Runtime runtime{{.reactors = 1, .offload_threads = 4}};
  std::unique_ptr<backend::GlusterBackend> be;
  backend::Cred root_cred{0, 0, {}};

  explicit Volume(bool jukebox = true, bool locks = true) {
    testing::FakeGfapi::set_root(dir.path);
    backend::fault::clear();
    runtime.start();
    backend::GlusterBackend::Config cfg;
    cfg.volume = "vol0";
    cfg.fsid = 9;
    cfg.jukebox = jukebox;
    cfg.native_locks = locks;
    cfg.servers.push_back({"gs1", 24007});
    auto made = backend::GlusterBackend::create(cfg, testing::FakeGfapi::api());
    ASSERT_TRUE(made.has_value());
    be = std::move(*made);
    auto started = run(runtime, be->start());
    ASSERT_TRUE(started.has_value());
  }
  ~Volume() {
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

TEST(Gluster, CapsHandlesAndResolve) {
  Volume v;
  auto caps = v.be->caps();
  EXPECT_TRUE(caps.has(backend::Cap::kStableHandles));
  EXPECT_TRUE(caps.has(backend::Cap::kNativeAccess));
  EXPECT_TRUE(caps.has(backend::Cap::kByteLocks));
  EXPECT_TRUE(caps.has(backend::Cap::kJukebox));
  EXPECT_TRUE(caps.has(backend::Cap::kSparseOps));
  EXPECT_TRUE(caps.has(backend::Cap::kCopyRange));
  EXPECT_FALSE(caps.has(backend::Cap::kNativeChange));
  EXPECT_FALSE(caps.has(backend::Cap::kCloneRange));
  EXPECT_TRUE(v.be->native_locks().has_value());
  EXPECT_EQ(v.be->fsid(), 9u);
  EXPECT_EQ(v.be->volume_id().size(), 32u);

  auto root = v.root();
  EXPECT_EQ(root->id().len, 17);  // tag + GFID
  EXPECT_EQ(root->type(), backend::FType::kDir);
  auto gfid = backend::GlusterBackend::gfid_from_oid(root->id());
  ASSERT_TRUE(gfid.has_value());
  EXPECT_TRUE(backend::GlusterBackend::oid_from_gfid(*gfid) == root->id());

  auto again = run(v.runtime, v.be->resolve(root->id()));
  ASSERT_TRUE(again.has_value());
  EXPECT_TRUE((*again)->id() == root->id());
  EXPECT_TRUE(v.be->stats().obj_hits >= 1);  // second resolve hit the handle cache

  // Malformed / foreign handle bytes → ESTALE, never a crash.
  std::array<std::byte, 21> local_like{};
  local_like[0] = std::byte{2};
  auto bad = run(v.runtime, v.be->resolve(*backend::ObjId::from(local_like)));
  EXPECT_FALSE(bad.has_value());
  EXPECT_EQ(raw(bad.error()), ESTALE);
  std::array<std::byte, 17> unknown{};
  unknown[0] = std::byte{3};
  unknown[1] = std::byte{0x7f};
  auto gone = run(v.runtime, v.be->resolve(*backend::ObjId::from(unknown)));
  EXPECT_FALSE(gone.has_value());
  EXPECT_EQ(raw(gone.error()), ESTALE);

  auto st = run(v.runtime, v.be->statfs());
  ASSERT_TRUE(st.has_value());
  EXPECT_TRUE(st->tbytes > 0);
}

TEST(Gluster, NamespaceOpsAndReaddirCookies) {
  Volume v;
  auto root = v.root();
  backend::SetAttr sa;
  sa.mode = 0750;
  auto dir = run(v.runtime, root->mkdir(v.root_cred, "d", sa));
  ASSERT_TRUE(dir.has_value());
  EXPECT_EQ(dir->attr.mode, 0750u);  // exact mode regardless of umask
  EXPECT_EQ(dir->obj->type(), backend::FType::kDir);

  backend::SetAttr fa;
  fa.mode = 0640;
  std::vector<std::string> names = {"a", "b", "c", "d", "e"};
  for (const auto& n : names) {
    auto f = run(v.runtime, dir->obj->create(v.root_cred, n, fa, nullptr));
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->attr.mode, 0640u);
    EXPECT_EQ(f->attr.type, backend::FType::kReg);
  }
  // lookup, ".", ".." at the export root clamps to the root
  auto a = run(v.runtime, dir->obj->lookup(v.root_cred, "a"));
  ASSERT_TRUE(a.has_value());
  auto up = run(v.runtime, root->lookup(v.root_cred, ".."));
  ASSERT_TRUE(up.has_value());
  EXPECT_TRUE((*up)->id() == root->id());
  auto back = run(v.runtime, dir->obj->lookup(v.root_cred, ".."));
  ASSERT_TRUE(back.has_value());
  EXPECT_TRUE((*back)->id() == root->id());
  EXPECT_FALSE(run(v.runtime, dir->obj->lookup(v.root_cred, "nope")).has_value());
  EXPECT_FALSE(run(v.runtime, dir->obj->lookup(v.root_cred, "a/b")).has_value());

  // readdir: page of 2, continue from the last cookie, no duplicates, no misses;
  // enriched entries carry attr + oid that resolve to the same object.
  std::set<std::string> seen;
  uint64_t cookie = 0;
  bool eof = false;
  int pages = 0;
  while (!eof) {
    auto page = run(v.runtime, dir->obj->readdir(v.root_cred, cookie, 2));
    ASSERT_TRUE(page.has_value());
    ++pages;
    for (const auto& e : page->ents) {
      EXPECT_TRUE(seen.insert(e.name).second);
      EXPECT_TRUE(e.attr.has_value());
      ASSERT_TRUE(e.oid.has_value());
      auto obj = run(v.runtime, v.be->resolve(*e.oid));
      ASSERT_TRUE(obj.has_value());
      auto attr = run(v.runtime, (*obj)->getattr());
      ASSERT_TRUE(attr.has_value());
      EXPECT_EQ(attr->fileid, e.fileid);
      cookie = e.cookie;
    }
    eof = page->eof;
    if (page->ents.empty()) EXPECT_TRUE(eof);
  }
  EXPECT_EQ(seen.size(), 5u);
  EXPECT_TRUE(pages >= 3);

  // rename within a directory, then across directories
  ASSERT_TRUE(run(v.runtime, dir->obj->rename(v.root_cred, "a", *dir->obj, "a2")).has_value());
  EXPECT_FALSE(run(v.runtime, dir->obj->lookup(v.root_cred, "a")).has_value());
  ASSERT_TRUE(run(v.runtime, dir->obj->rename(v.root_cred, "a2", *root, "a3")).has_value());
  auto a3 = run(v.runtime, root->lookup(v.root_cred, "a3"));
  ASSERT_TRUE(a3.has_value());
  EXPECT_TRUE((*a3)->id() == (*a)->id());  // same inode → same handle (P1)

  // hard link + nlink, symlink + readlink, mknod fifo
  ASSERT_TRUE(run(v.runtime, dir->obj->link(v.root_cred, **a3, "alink")).has_value());
  auto linked = run(v.runtime, (*a3)->getattr());
  ASSERT_TRUE(linked.has_value());
  EXPECT_EQ(linked->nlink, 2u);
  auto sl = run(v.runtime, dir->obj->symlink(v.root_cred, "sl", "../a3", backend::SetAttr{}));
  ASSERT_TRUE(sl.has_value());
  EXPECT_EQ(sl->obj->type(), backend::FType::kLnk);
  auto target = run(v.runtime, sl->obj->readlink());
  ASSERT_TRUE(target.has_value());
  EXPECT_STREQ(*target, "../a3");
  auto fifo = run(v.runtime, dir->obj->mknod(v.root_cred, "ff", backend::FType::kFifo, {},
                                             backend::SetAttr{}));
  ASSERT_TRUE(fifo.has_value());
  EXPECT_EQ(fifo->obj->type(), backend::FType::kFifo);

  // unlink/rmdir type discipline
  auto sub = run(v.runtime, dir->obj->mkdir(v.root_cred, "sub", backend::SetAttr{}));
  ASSERT_TRUE(sub.has_value());
  auto eisdir = run(v.runtime, dir->obj->unlink(v.root_cred, "sub"));
  EXPECT_FALSE(eisdir.has_value());
  EXPECT_EQ(raw(eisdir.error()), EISDIR);
  auto enotdir = run(v.runtime, dir->obj->rmdir(v.root_cred, "b"));
  EXPECT_FALSE(enotdir.has_value());
  EXPECT_EQ(raw(enotdir.error()), ENOTDIR);
  ASSERT_TRUE(run(v.runtime, dir->obj->rmdir(v.root_cred, "sub")).has_value());
  ASSERT_TRUE(run(v.runtime, dir->obj->unlink(v.root_cred, "b")).has_value());
  auto noent = run(v.runtime, dir->obj->lookup(v.root_cred, "b"));
  EXPECT_FALSE(noent.has_value());
  EXPECT_EQ(raw(noent.error()), ENOENT);

  // setattr: mode / size / client times
  backend::SetAttr s2;
  s2.mode = 0600;
  s2.size = 100;
  s2.mtime_how = backend::SetAttr::TimeHow::kClient;
  s2.mtime = {1234567, 0};
  auto after = run(v.runtime, (*a3)->setattr(v.root_cred, s2));
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->mode, 0600u);
  EXPECT_EQ(after->size, 100u);
  EXPECT_EQ(after->mtime.sec, 1234567);
  auto dir_size = run(v.runtime, dir->obj->setattr(v.root_cred, s2));
  EXPECT_FALSE(dir_size.has_value());
  EXPECT_EQ(raw(dir_size.error()), EISDIR);
}

TEST(Gluster, RecreatedObjectGetsNewHandle) {
  Volume v;
  auto root = v.root();
  auto f1 = run(v.runtime, root->create(v.root_cred, "f", backend::SetAttr{}, nullptr));
  ASSERT_TRUE(f1.has_value());
  auto oid1 = f1->obj->id();
  f1->obj.reset();
  v.be->flush_fd_cache();
  ASSERT_TRUE(run(v.runtime, root->unlink(v.root_cred, "f")).has_value());
  auto f2 = run(v.runtime, root->create(v.root_cred, "f", backend::SetAttr{}, nullptr));
  ASSERT_TRUE(f2.has_value());
  EXPECT_FALSE(f2->obj->id() == oid1);  // P2
  auto stale = run(v.runtime, v.be->resolve(oid1));
  EXPECT_FALSE(stale.has_value());
  EXPECT_EQ(raw(stale.error()), ESTALE);
}

TEST(Gluster, ExclusiveCreateReplay) {
  Volume v;
  auto root = v.root();
  backend::ExclVerf verf{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                         std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
  auto first = run(v.runtime, root->create(v.root_cred, "x", backend::SetAttr{}, &verf));
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->attr.mode, 0u);  // EXCLUSIVE leaves mode for the follow-up SETATTR
  auto replay = run(v.runtime, root->create(v.root_cred, "x", backend::SetAttr{}, &verf));
  ASSERT_TRUE(replay.has_value());
  EXPECT_TRUE(replay->obj->id() == first->obj->id());
  backend::ExclVerf other = verf;
  other[0] = std::byte{9};
  auto conflict = run(v.runtime, root->create(v.root_cred, "x", backend::SetAttr{}, &other));
  EXPECT_FALSE(conflict.has_value());
  EXPECT_EQ(raw(conflict.error()), EEXIST);
  auto plain = run(v.runtime, root->create(v.root_cred, "x", backend::SetAttr{}, nullptr));
  EXPECT_FALSE(plain.has_value());
  EXPECT_EQ(raw(plain.error()), EEXIST);
}

TEST(Gluster, AnonymousAndOpenStateIo) {
  Volume v;
  auto root = v.root();
  backend::SetAttr fa;
  fa.mode = 0644;
  auto f = run(v.runtime, root->create(v.root_cred, "io", fa, nullptr));
  ASSERT_TRUE(f.has_value());
  auto& obj = *f->obj;
  backend::OpenCtx anon{v.root_cred};

  auto w = run(v.runtime, obj.write(anon, 0, v.bytes("hello "), backend::Stability::kUnstable));
  ASSERT_TRUE(w.has_value());
  EXPECT_EQ(*w, 6u);
  // scatter write straddling two segments, file-sync stability
  std::string s1 = "wor", s2 = "ld!";
  iovec iov[2] = {{s1.data(), s1.size()}, {s2.data(), s2.size()}};
  auto w2 = run(v.runtime, obj.write(anon, 6, std::span<const iovec>(iov, 2),
                                     backend::Stability::kFileSync));
  ASSERT_TRUE(w2.has_value());
  EXPECT_EQ(*w2, 6u);
  EXPECT_STREQ(v.read_all(obj), "hello world!");
  std::vector<std::byte> buf(5);
  bool eof = true;
  auto r = run(v.runtime, obj.read(anon, 0, std::span(buf), eof));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 5u);
  EXPECT_FALSE(eof);
  auto tail = run(v.runtime, obj.read(anon, 10, std::span(buf), eof));
  ASSERT_TRUE(tail.has_value());
  EXPECT_EQ(*tail, 2u);
  EXPECT_TRUE(eof);
  auto zero = run(v.runtime, obj.read(anon, 12, std::span<std::byte>{}, eof));
  ASSERT_TRUE(zero.has_value());
  EXPECT_TRUE(eof);
  EXPECT_TRUE(run(v.runtime, obj.commit(anon, 0, 0)).has_value());
  auto st = v.be->stats();
  EXPECT_TRUE(st.fd_upgrades >= 1 || st.fd_misses >= 1);

  // v4 open state: IO through the OPEN's own descriptor; a read-only open state
  // used for a write falls back to the anonymous path (same-owner merge upgrade).
  backend::OpenFlags rd;
  rd.set(backend::OpenFlag::kRead);
  auto ro = run(v.runtime, obj.open(v.root_cred, rd));
  ASSERT_TRUE(ro.has_value());
  backend::OpenCtx via_open{v.root_cred, ro->get()};
  auto r2 = run(v.runtime, obj.read(via_open, 6, std::span(buf), eof));
  ASSERT_TRUE(r2.has_value());
  EXPECT_STREQ(std::string(reinterpret_cast<char*>(buf.data()), 5), "world");
  auto w3 = run(v.runtime, obj.write(via_open, 0, v.bytes("HELLO"), backend::Stability::kDataSync));
  ASSERT_TRUE(w3.has_value());
  EXPECT_STREQ(v.read_all(obj), "HELLO world!");
  backend::OpenFlags wr;
  wr.set(backend::OpenFlag::kRead).set(backend::OpenFlag::kWrite);
  auto rw = run(v.runtime, obj.open(v.root_cred, wr));
  ASSERT_TRUE(rw.has_value());
  backend::OpenCtx via_rw{v.root_cred, rw->get()};
  ASSERT_TRUE(run(v.runtime, obj.write(via_rw, 12, v.bytes("?"), backend::Stability::kUnstable)).has_value());
  EXPECT_TRUE(run(v.runtime, obj.commit(via_rw, 0, 0)).has_value());
  EXPECT_STREQ(v.read_all(obj), "HELLO world!?");
  // open() on a directory is not degraded to EOPNOTSUPP: it is a real EISDIR
  auto dopen = run(v.runtime, root->open(v.root_cred, rd));
  EXPECT_FALSE(dopen.has_value());
  EXPECT_EQ(raw(dopen.error()), EISDIR);

  // sticky commit poison after an injected sync failure; operator clear
  backend::fault::arm(backend::fault::Kind::kFsyncEio, 1);
  auto bad = run(v.runtime, obj.commit(anon, 0, 0));
  EXPECT_FALSE(bad.has_value());
  EXPECT_EQ(raw(bad.error()), EIO);
  auto still = run(v.runtime, obj.commit(anon, 0, 0));
  EXPECT_FALSE(still.has_value());
  EXPECT_TRUE(v.be->is_poisoned(obj.id()));
  EXPECT_EQ(v.be->clear_poison(), 1u);
  EXPECT_TRUE(run(v.runtime, obj.commit(anon, 0, 0)).has_value());
  ro->reset();
  rw->reset();
}

TEST(Gluster, SparseOpsAndCopyRange) {
  Volume v;
  auto root = v.root();
  auto f = run(v.runtime, root->create(v.root_cred, "sp", backend::SetAttr{}, nullptr));
  ASSERT_TRUE(f.has_value());
  auto& obj = *f->obj;
  backend::OpenCtx anon{v.root_cred};
  std::string data(8192, 'x');
  ASSERT_TRUE(run(v.runtime, obj.write(anon, 0, v.bytes(data), backend::Stability::kFileSync)).has_value());
  ASSERT_TRUE(run(v.runtime, obj.allocate(anon, 8192, 4096)).has_value());
  auto grown = run(v.runtime, obj.getattr());
  ASSERT_TRUE(grown.has_value());
  EXPECT_EQ(grown->size, 12288u);
  auto hole_at_end = run(v.runtime, obj.seek(anon, 0, backend::SeekWhat::kHole));
  ASSERT_TRUE(hole_at_end.has_value());
  EXPECT_TRUE(*hole_at_end >= 8192u);
  auto past = run(v.runtime, obj.seek(anon, 20000, backend::SeekWhat::kData));
  EXPECT_FALSE(past.has_value());
  EXPECT_EQ(raw(past.error()), ENXIO);
  // punch a hole in the middle (tmpfs and ext4 both support it); size unchanged
  auto punched = run(v.runtime, obj.deallocate(anon, 0, 4096));
  ASSERT_TRUE(punched.has_value());
  auto same = run(v.runtime, obj.getattr());
  ASSERT_TRUE(same.has_value());
  EXPECT_EQ(same->size, 12288u);
  std::vector<std::byte> buf(4);
  bool eof = false;
  auto zeros = run(v.runtime, obj.read(anon, 0, std::span(buf), eof));
  ASSERT_TRUE(zeros.has_value());
  EXPECT_EQ(static_cast<int>(buf[0]), 0);

  auto dst = run(v.runtime, root->create(v.root_cred, "cp", backend::SetAttr{}, nullptr));
  ASSERT_TRUE(dst.has_value());
  auto copied = run(v.runtime, obj.copy_range(anon, *dst->obj, anon, 4096, 0, 4096));
  ASSERT_TRUE(copied.has_value());
  EXPECT_EQ(*copied, 4096u);
  EXPECT_EQ(v.read_all(*dst->obj, 8192).size(), 4096u);
  auto to_eof = run(v.runtime, obj.copy_range(anon, *dst->obj, anon, 4096, 4096, 0));
  ASSERT_TRUE(to_eof.has_value());
  EXPECT_EQ(*to_eof, 8192u);
  auto dattr = run(v.runtime, dst->obj->getattr());
  ASSERT_TRUE(dattr.has_value());
  EXPECT_EQ(dattr->size, 12288u);
}

TEST(Gluster, NativeAccessRunsUnderCallerIdentity) {
  Volume v;
  auto root = v.root();
  backend::SetAttr fa;
  fa.mode = 0600;
  auto f = run(v.runtime, root->create(v.root_cred, "priv", fa, nullptr));
  ASSERT_TRUE(f.has_value());
  uint32_t me = static_cast<uint32_t>(getuid());
  uint32_t gid = static_cast<uint32_t>(getgid());
  uint32_t other = me + 1000;
  uint32_t groups[] = {gid, 4242};
  backend::Cred owner{me, gid, groups};
  backend::Cred stranger{other, other, {}};

  backend::AccessMask all;
  all.set(backend::Access::kRead).set(backend::Access::kModify).set(backend::Access::kExecute)
      .set(backend::Access::kLookup).set(backend::Access::kDelete).set(backend::Access::kExtend);
  uint64_t before = testing::FakeGfapi::access_calls();
  auto mine = run(v.runtime, f->obj->access(owner, all));
  ASSERT_TRUE(mine.has_value());
  EXPECT_TRUE(mine->has(backend::Access::kRead));
  EXPECT_TRUE(mine->has(backend::Access::kModify));
  EXPECT_FALSE(mine->has(backend::Access::kExecute));
  EXPECT_FALSE(mine->has(backend::Access::kLookup));  // not a directory
  EXPECT_FALSE(mine->has(backend::Access::kDelete));  // not a directory
  EXPECT_EQ(testing::FakeGfapi::access_calls() - before, 3u);  // R, W, X: one each
  EXPECT_EQ(testing::FakeGfapi::last_fsuid(), me);
  EXPECT_EQ(testing::FakeGfapi::last_fsgid(), gid);

  auto theirs = run(v.runtime, f->obj->access(stranger, all));
  ASSERT_TRUE(theirs.has_value());
  EXPECT_FALSE(theirs->has(backend::Access::kRead));
  EXPECT_EQ(testing::FakeGfapi::last_fsuid(), other);

  // Anonymous IO is gated by the same native check; the owner keeps the v3
  // open-less relaxation even when mode bits say no.
  std::vector<std::byte> buf(4);
  bool eof = false;
  auto denied = run(v.runtime, f->obj->read({stranger}, 0, std::span(buf), eof));
  EXPECT_FALSE(denied.has_value());
  EXPECT_EQ(raw(denied.error()), EACCES);
  backend::SetAttr locked;
  locked.mode = 0000;
  ASSERT_TRUE(run(v.runtime, f->obj->setattr(owner, locked)).has_value());
  auto relaxed = run(v.runtime, f->obj->read({owner}, 0, std::span(buf), eof));
  EXPECT_TRUE(relaxed.has_value());
  // ... but the v4 OPEN itself is authoritative: no permission → EACCES, no degrade.
  backend::OpenFlags rd;
  rd.set(backend::OpenFlag::kRead);
  auto o = run(v.runtime, f->obj->open(owner, rd));
  EXPECT_FALSE(o.has_value());
  EXPECT_EQ(raw(o.error()), EACCES);
  // Directory bits: the export root is 0700 for the test user, so the owner may
  // look up and delete, a stranger may not.
  auto dir_mine = run(v.runtime, root->access(owner, all));
  ASSERT_TRUE(dir_mine.has_value());
  EXPECT_TRUE(dir_mine->has(backend::Access::kLookup));
  EXPECT_TRUE(dir_mine->has(backend::Access::kDelete));
  EXPECT_FALSE(dir_mine->has(backend::Access::kExecute));
  auto dir_as = run(v.runtime, root->access(stranger, all));
  ASSERT_TRUE(dir_as.has_value());
  EXPECT_FALSE(dir_as->has(backend::Access::kLookup));
  EXPECT_FALSE(dir_as->has(backend::Access::kDelete));
}

TEST(Gluster, JukeboxMapping) {
  {
    Volume v(/*jukebox=*/true);
    auto root = v.root();
    auto f = run(v.runtime, root->create(v.root_cred, "j", backend::SetAttr{}, nullptr));
    ASSERT_TRUE(f.has_value());
    std::vector<std::byte> buf(4);
    bool eof = false;
    // first IO opens the glfd (one fop) — inject the failure on the read itself
    ASSERT_TRUE(run(v.runtime, f->obj->read({v.root_cred}, 0, std::span(buf), eof)).has_value());
    testing::FakeGfapi::fail_next(ENOTCONN, 1);
    auto r = run(v.runtime, f->obj->read({v.root_cred}, 0, std::span(buf), eof));
    EXPECT_FALSE(r.has_value());
    EXPECT_TRUE(r.error() == Errno::kJukebox);
    EXPECT_EQ(v.be->stats().jukebox, 1u);
    // recovered: the next IO succeeds
    EXPECT_TRUE(run(v.runtime, f->obj->read({v.root_cred}, 0, std::span(buf), eof)).has_value());
    // metadata path too (getattr → glfs_h_stat)
    testing::FakeGfapi::fail_next(ETIMEDOUT, 1);
    auto a = run(v.runtime, f->obj->getattr());
    EXPECT_FALSE(a.has_value());
    EXPECT_TRUE(a.error() == Errno::kJukebox);
    // the shared fault-injection kind reaches this backend as well
    backend::fault::arm(backend::fault::Kind::kJukebox, 1);
    auto fj = run(v.runtime, f->obj->read({v.root_cred}, 0, std::span(buf), eof));
    EXPECT_FALSE(fj.has_value());
    EXPECT_TRUE(fj.error() == Errno::kJukebox);
    // ordinary errors stay themselves
    testing::FakeGfapi::fail_next(ENOSPC, 1);
    auto w = run(v.runtime, f->obj->write({v.root_cred}, 0, v.bytes("x"), backend::Stability::kUnstable));
    EXPECT_FALSE(w.has_value());
    EXPECT_EQ(raw(w.error()), ENOSPC);
  }
  {
    Volume v(/*jukebox=*/false);
    EXPECT_FALSE(v.be->caps().has(backend::Cap::kJukebox));
    auto root = v.root();
    testing::FakeGfapi::fail_next(ENOTCONN, 1);
    auto a = run(v.runtime, root->getattr());
    EXPECT_FALSE(a.has_value());
    EXPECT_EQ(raw(a.error()), EIO);
  }
}

TEST(Gluster, NativeByteRangeLocks) {
  Volume v;
  auto root = v.root();
  auto f = run(v.runtime, root->create(v.root_cred, "lk", backend::SetAttr{}, nullptr));
  ASSERT_TRUE(f.has_value());
  auto& mgr = v.be->native_locks()->get();
  backend::LockOwnerId a, b;
  a.len = 4;
  std::memcpy(a.bytes.data(), "own1", 4);
  b.len = 4;
  std::memcpy(b.bytes.data(), "own2", 4);
  auto& obj = *f->obj;

  ASSERT_TRUE(run(v.runtime, mgr.lock(obj, a, {0, 10}, true, false)).has_value());
  EXPECT_EQ(v.be->stats().lock_fds, 1u);
  auto conflict = run(v.runtime, mgr.lock(obj, b, {5, 10}, true, false));
  EXPECT_FALSE(conflict.has_value());
  EXPECT_EQ(raw(conflict.error()), EAGAIN);
  auto probe = run(v.runtime, mgr.test(obj, {5, 1}, false));
  ASSERT_TRUE(probe.has_value());
  ASSERT_TRUE(probe->has_value());
  EXPECT_TRUE((*probe)->exclusive);
  EXPECT_EQ((*probe)->range.offset, 0u);
  EXPECT_EQ((*probe)->range.length, 10u);
  auto clear = run(v.runtime, mgr.test(obj, {10, UINT64_MAX}, true));
  ASSERT_TRUE(clear.has_value());
  EXPECT_FALSE(clear->has_value());
  // shared locks coexist; same owner re-locks freely
  ASSERT_TRUE(run(v.runtime, mgr.lock(obj, b, {20, 5}, false, false)).has_value());
  ASSERT_TRUE(run(v.runtime, mgr.lock(obj, a, {20, 5}, false, false)).has_value());
  ASSERT_TRUE(run(v.runtime, mgr.lock(obj, a, {0, 10}, true, false)).has_value());
  // unlock part, the rest still blocks
  ASSERT_TRUE(run(v.runtime, mgr.unlock(obj, a, {0, 5})).has_value());
  ASSERT_TRUE(run(v.runtime, mgr.lock(obj, b, {0, 5}, true, false)).has_value());
  EXPECT_FALSE(run(v.runtime, mgr.lock(obj, b, {5, 5}, true, false)).has_value());
  // release closes the owner's descriptor and frees everything it held
  ASSERT_TRUE(run(v.runtime, mgr.release(obj, a)).has_value());
  EXPECT_EQ(v.be->stats().lock_fds, 1u);
  ASSERT_TRUE(run(v.runtime, mgr.lock(obj, b, {5, 5}, true, false)).has_value());
  ASSERT_TRUE(run(v.runtime, mgr.release(obj, b)).has_value());
  EXPECT_EQ(v.be->stats().lock_fds, 0u);
  // unlocking what was never held is fine; to-EOF ranges are accepted
  EXPECT_TRUE(run(v.runtime, mgr.unlock(obj, a, {0, UINT64_MAX})).has_value());
  EXPECT_TRUE(run(v.runtime, mgr.lock(obj, a, {100, UINT64_MAX}, true, false)).has_value());
  auto eof_conf = run(v.runtime, mgr.test(obj, {1000, 1}, false));
  ASSERT_TRUE(eof_conf.has_value());
  ASSERT_TRUE(eof_conf->has_value());
  EXPECT_EQ((*eof_conf)->range.length, UINT64_MAX);
  // locks on a directory are meaningless
  auto on_dir = run(v.runtime, mgr.lock(*root, a, {0, 1}, true, false));
  EXPECT_FALSE(on_dir.has_value());
  EXPECT_EQ(raw(on_dir.error()), EINVAL);
}

TEST(Gluster, StopRestartAndLeakFree) {
  int objects_before = testing::FakeGfapi::live_objects();
  int fds_before = testing::FakeGfapi::live_fds();
  {
    Volume v;
    auto root = v.root();
    auto f = run(v.runtime, root->create(v.root_cred, "r", backend::SetAttr{}, nullptr));
    ASSERT_TRUE(f.has_value());
    ASSERT_TRUE(run(v.runtime, f->obj->write({v.root_cred}, 0, v.bytes("abc"), backend::Stability::kUnstable)).has_value());
    auto oid = f->obj->id();
    f->obj.reset();
    root.reset();
    ASSERT_TRUE(run(v.runtime, v.be->stop()).has_value());
    EXPECT_FALSE(v.be->started());
    auto down = run(v.runtime, v.be->root());
    EXPECT_FALSE(down.has_value());
    EXPECT_EQ(raw(down.error()), ENOTCONN);
    ASSERT_TRUE(run(v.runtime, v.be->start()).has_value());
    auto back = run(v.runtime, v.be->resolve(oid));  // GFIDs survive a reconnect (P1)
    ASSERT_TRUE(back.has_value());
    EXPECT_STREQ(v.read_all(**back), "abc");
    back->reset();
    // a volume that refuses to come up fails start() cleanly
    ASSERT_TRUE(run(v.runtime, v.be->stop()).has_value());
    testing::FakeGfapi::fail_init(ECONNREFUSED);
    auto refused = run(v.runtime, v.be->start());
    EXPECT_FALSE(refused.has_value());
    EXPECT_EQ(raw(refused.error()), ECONNREFUSED);
    testing::FakeGfapi::fail_init(0);
  }
  EXPECT_EQ(testing::FakeGfapi::live_objects(), objects_before);
  EXPECT_EQ(testing::FakeGfapi::live_fds(), fds_before);
}

TEST(Gluster, ConfigFactory) {
  backend::register_builtin_backends();
  const auto* factory = backend::find_backend("gluster");
  ASSERT_TRUE(factory != nullptr);
  EXPECT_TRUE(factory->virtual_path);
  EXPECT_FALSE(backend::find_backend("local")->virtual_path);

  backend::BackendConfig cfg;
  cfg.path = "/data";
  cfg.fsid = 5;
  cfg.values["volume"] = "vol0";
  cfg.values["servers"] = "gs1, gs2:24008,[fd00::1]:24009";
  cfg.values["subdir"] = "/exports/a";
  cfg.values["fd_cache"] = "128";
  cfg.values["readdir_enrich"] = "false";
  cfg.values["jukebox"] = "false";
  cfg.values["native_locks"] = "false";
  cfg.values["log_level"] = "7";
  auto made = factory->make(cfg);
  ASSERT_TRUE(made != nullptr);
  auto* g = dynamic_cast<backend::GlusterBackend*>(made.get());
  ASSERT_TRUE(g != nullptr);
  EXPECT_STREQ(g->config().volume, "vol0");
  EXPECT_EQ(g->config().servers.size(), 3u);
  EXPECT_STREQ(g->config().servers[0].host, "gs1");
  EXPECT_EQ(g->config().servers[0].port, 24007);
  EXPECT_STREQ(g->config().servers[1].host, "gs2");
  EXPECT_EQ(g->config().servers[1].port, 24008);
  EXPECT_STREQ(g->config().servers[2].host, "fd00::1");
  EXPECT_EQ(g->config().servers[2].port, 24009);
  EXPECT_STREQ(g->config().subdir, "/exports/a");
  EXPECT_EQ(g->config().fd_cache, 128u);
  EXPECT_FALSE(g->config().enrich_readdir);
  EXPECT_FALSE(g->caps().has(backend::Cap::kJukebox));
  EXPECT_FALSE(g->caps().has(backend::Cap::kByteLocks));
  EXPECT_FALSE(g->native_locks().has_value());
  EXPECT_FALSE(g->started());

  backend::BackendConfig missing;
  missing.path = "/data";
  missing.fsid = 5;
  EXPECT_TRUE(factory->make(missing) == nullptr);  // volume is required
  backend::BackendConfig bad = cfg;
  bad.values["servers"] = "gs1:notaport";
  EXPECT_TRUE(factory->make(bad) == nullptr);
  backend::BackendConfig unknown = cfg;
  unknown.values["volfile"] = "x";
  EXPECT_TRUE(factory->make(unknown) == nullptr);
  backend::BackendConfig relative = cfg;
  relative.values["subdir"] = "exports";
  EXPECT_TRUE(factory->make(relative) == nullptr);

  // Without libgfapi on this host start() reports the missing library, no crash.
  backend::GlusterBackend::Config plain;
  plain.volume = "vol0";
  plain.fsid = 1;
  auto sys = backend::GlusterBackend::create(plain);
  ASSERT_TRUE(sys.has_value());
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  auto started = run(runtime, (*sys)->start());
  std::string detail;
  auto lib = backend::gfapi::load_system_api(&detail);
  if (!lib) {
    EXPECT_FALSE(started.has_value());
    EXPECT_EQ(started.error(), lib.error());
  }
  if (started) (void)run(runtime, (*sys)->stop());
  sys->reset();
  runtime.stop_and_join();
}
