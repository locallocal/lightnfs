// CephFS backend (plan doc 10 §5.3) over the in-process libcephfs fake: the design 05
// §5.9 implementer checklist, exercised end to end through the real backend code —
// handle codec (P1/P2/ESTALE), namespace ops, EXCLUSIVE replay, readdir cookies with
// enrichment, anonymous + open-state IO, sticky commit poison, v4.2 ops, the native
// change counter, identity plumbing (opens under the caller), jukebox/blocklist
// mapping, native byte-range locks and the config factory.

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

#include "backend/cephfs/cephfs.hpp"
#include "backend/fault.hpp"
#include "cephapi_fake.hpp"
#include "runtime/runtime.hpp"

using namespace lnfs;

namespace {

struct TmpDir {
  std::string path;
  TmpDir() {
    char tmpl[] = "/tmp/lnfs-cephfs-XXXXXX";
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

// A started fake-backed CephFS export.
struct Mount {
  TmpDir dir;
  rt::Runtime runtime{{.reactors = 1, .offload_threads = 4}};
  std::unique_ptr<backend::CephBackend> be;
  backend::Cred root_cred{0, 0, {}};

  explicit Mount(bool jukebox = true, bool locks = true) {
    testing::FakeCephApi::set_root(dir.path);
    backend::fault::clear();
    runtime.start();
    backend::CephBackend::Config cfg;
    cfg.fsid = 9;
    cfg.fs_name = "cephfs";
    cfg.id = "lightnfs";
    cfg.mon_host = "10.0.0.1";
    cfg.jukebox = jukebox;
    cfg.native_locks = locks;
    auto made = backend::CephBackend::create(cfg, testing::FakeCephApi::api());
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

TEST(Cephfs, CapsHandlesAndResolve) {
  Mount m;
  auto caps = m.be->caps();
  EXPECT_TRUE(caps.has(backend::Cap::kStableHandles));
  EXPECT_TRUE(caps.has(backend::Cap::kNativeChange));  // stx_version
  EXPECT_FALSE(caps.has(backend::Cap::kNativeAccess));  // no ceph_ll_access
  EXPECT_TRUE(caps.has(backend::Cap::kByteLocks));
  EXPECT_TRUE(caps.has(backend::Cap::kJukebox));
  EXPECT_TRUE(caps.has(backend::Cap::kSparseOps));
  EXPECT_TRUE(caps.has(backend::Cap::kCopyRange));
  EXPECT_FALSE(caps.has(backend::Cap::kCloneRange));
  EXPECT_TRUE(m.be->native_locks().has_value());
  EXPECT_EQ(m.be->fsid(), 9u);
  EXPECT_EQ(m.be->fscid(), 7);
  EXPECT_FALSE(m.be->cluster_fsid().empty());

  auto root = m.root();
  EXPECT_EQ(root->id().len, 17);  // tag + ino + snapid
  EXPECT_EQ(static_cast<int>(root->id().bytes[0]), 5);
  EXPECT_EQ(root->type(), backend::FType::kDir);
  auto vino = backend::CephBackend::vino_from_oid(root->id());
  ASSERT_TRUE(vino.has_value());
  EXPECT_EQ(vino->snapid, backend::cephapi::kNoSnap);
  EXPECT_TRUE(backend::CephBackend::oid_from_vino(*vino) == root->id());

  auto again = run(m.runtime, m.be->resolve(root->id()));
  ASSERT_TRUE(again.has_value());
  EXPECT_TRUE((*again)->id() == root->id());
  EXPECT_TRUE(m.be->stats().obj_hits >= 1);  // second resolve hit the handle cache

  // Malformed / foreign handle bytes → ESTALE, never a crash (and no cluster trip).
  std::array<std::byte, 21> local_like{};
  local_like[0] = std::byte{2};
  auto bad = run(m.runtime, m.be->resolve(*backend::ObjId::from(local_like)));
  EXPECT_FALSE(bad.has_value());
  EXPECT_EQ(raw(bad.error()), ESTALE);
  std::array<std::byte, 17> gluster_like{};
  gluster_like[0] = std::byte{3};
  auto foreign = run(m.runtime, m.be->resolve(*backend::ObjId::from(gluster_like)));
  EXPECT_FALSE(foreign.has_value());
  EXPECT_EQ(raw(foreign.error()), ESTALE);
  std::array<std::byte, 17> zero_ino{};
  zero_ino[0] = std::byte{5};
  auto none = run(m.runtime, m.be->resolve(*backend::ObjId::from(zero_ino)));
  EXPECT_FALSE(none.has_value());
  EXPECT_EQ(raw(none.error()), ESTALE);
  std::array<std::byte, 17> unknown{};
  unknown[0] = std::byte{5};
  unknown[1] = std::byte{0x7f};
  unknown[9] = std::byte{0xfe};  // snapid NOSNAP low byte
  auto gone = run(m.runtime, m.be->resolve(*backend::ObjId::from(unknown)));
  EXPECT_FALSE(gone.has_value());
  EXPECT_EQ(raw(gone.error()), ESTALE);

  auto st = run(m.runtime, m.be->statfs());
  ASSERT_TRUE(st.has_value());
  EXPECT_TRUE(st->tbytes > 0);
}

TEST(Cephfs, NamespaceOpsAndReaddirCookies) {
  Mount m;
  auto root = m.root();
  backend::SetAttr sa;
  sa.mode = 0750;
  auto dir = run(m.runtime, root->mkdir(m.root_cred, "d", sa));
  ASSERT_TRUE(dir.has_value());
  EXPECT_EQ(dir->attr.mode, 0750u);  // exact mode regardless of umask
  EXPECT_EQ(dir->obj->type(), backend::FType::kDir);

  backend::SetAttr fa;
  fa.mode = 0640;
  std::vector<std::string> names = {"a", "b", "c", "d", "e"};
  for (const auto& n : names) {
    auto f = run(m.runtime, dir->obj->create(m.root_cred, n, fa, nullptr));
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->attr.mode, 0640u);
    EXPECT_EQ(f->attr.type, backend::FType::kReg);
  }
  EXPECT_EQ(testing::FakeCephApi::live_fhs(), 0);  // create's Fh is closed right away
  // lookup, ".", ".." at the export root clamps to the root
  auto a = run(m.runtime, dir->obj->lookup(m.root_cred, "a"));
  ASSERT_TRUE(a.has_value());
  auto up = run(m.runtime, root->lookup(m.root_cred, ".."));
  ASSERT_TRUE(up.has_value());
  EXPECT_TRUE((*up)->id() == root->id());
  auto back = run(m.runtime, dir->obj->lookup(m.root_cred, ".."));
  ASSERT_TRUE(back.has_value());
  EXPECT_TRUE((*back)->id() == root->id());
  EXPECT_FALSE(run(m.runtime, dir->obj->lookup(m.root_cred, "nope")).has_value());
  EXPECT_FALSE(run(m.runtime, dir->obj->lookup(m.root_cred, "a/b")).has_value());

  // readdir: page of 2, continue from the last cookie, no duplicates, no misses;
  // enriched entries carry attr + oid that resolve to the same object without a
  // per-entry lookup.
  std::set<std::string> seen;
  uint64_t cookie = 0;
  bool eof = false;
  int pages = 0;
  while (!eof) {
    auto page = run(m.runtime, dir->obj->readdir(m.root_cred, cookie, 2));
    ASSERT_TRUE(page.has_value());
    ++pages;
    for (const auto& e : page->ents) {
      EXPECT_TRUE(seen.insert(e.name).second);
      EXPECT_TRUE(e.attr.has_value());
      ASSERT_TRUE(e.oid.has_value());
      auto obj = run(m.runtime, m.be->resolve(*e.oid));
      ASSERT_TRUE(obj.has_value());
      auto attr = run(m.runtime, (*obj)->getattr());
      ASSERT_TRUE(attr.has_value());
      EXPECT_EQ(attr->fileid, e.fileid);
      EXPECT_EQ(attr->change, e.attr->change);
      cookie = e.cookie;
    }
    eof = page->eof;
    if (page->ents.empty()) EXPECT_TRUE(eof);
  }
  EXPECT_EQ(seen.size(), 5u);
  EXPECT_TRUE(pages >= 3);
  EXPECT_EQ(testing::FakeCephApi::live_dirs(), 0);

  // rename within a directory, then across directories
  ASSERT_TRUE(run(m.runtime, dir->obj->rename(m.root_cred, "a", *dir->obj, "a2")).has_value());
  EXPECT_FALSE(run(m.runtime, dir->obj->lookup(m.root_cred, "a")).has_value());
  ASSERT_TRUE(run(m.runtime, dir->obj->rename(m.root_cred, "a2", *root, "a3")).has_value());
  auto a3 = run(m.runtime, root->lookup(m.root_cred, "a3"));
  ASSERT_TRUE(a3.has_value());
  EXPECT_TRUE((*a3)->id() == (*a)->id());  // same inode → same handle (P1)

  // hard link + nlink, symlink + readlink, mknod fifo
  ASSERT_TRUE(run(m.runtime, dir->obj->link(m.root_cred, **a3, "alink")).has_value());
  auto linked = run(m.runtime, (*a3)->getattr());
  ASSERT_TRUE(linked.has_value());
  EXPECT_EQ(linked->nlink, 2u);
  auto sl = run(m.runtime, dir->obj->symlink(m.root_cred, "sl", "../a3", backend::SetAttr{}));
  ASSERT_TRUE(sl.has_value());
  EXPECT_EQ(sl->obj->type(), backend::FType::kLnk);
  auto target = run(m.runtime, sl->obj->readlink());
  ASSERT_TRUE(target.has_value());
  EXPECT_STREQ(*target, "../a3");
  auto fifo = run(m.runtime, dir->obj->mknod(m.root_cred, "ff", backend::FType::kFifo, {},
                                             backend::SetAttr{}));
  ASSERT_TRUE(fifo.has_value());
  EXPECT_EQ(fifo->obj->type(), backend::FType::kFifo);

  // unlink/rmdir type discipline
  auto sub = run(m.runtime, dir->obj->mkdir(m.root_cred, "sub", backend::SetAttr{}));
  ASSERT_TRUE(sub.has_value());
  auto eisdir = run(m.runtime, dir->obj->unlink(m.root_cred, "sub"));
  EXPECT_FALSE(eisdir.has_value());
  EXPECT_EQ(raw(eisdir.error()), EISDIR);
  auto enotdir = run(m.runtime, dir->obj->rmdir(m.root_cred, "b"));
  EXPECT_FALSE(enotdir.has_value());
  EXPECT_EQ(raw(enotdir.error()), ENOTDIR);
  ASSERT_TRUE(run(m.runtime, dir->obj->rmdir(m.root_cred, "sub")).has_value());
  ASSERT_TRUE(run(m.runtime, dir->obj->unlink(m.root_cred, "b")).has_value());
  auto noent = run(m.runtime, dir->obj->lookup(m.root_cred, "b"));
  EXPECT_FALSE(noent.has_value());
  EXPECT_EQ(raw(noent.error()), ENOENT);

  // setattr: mode / size / client mtime / server atime
  backend::SetAttr s2;
  s2.mode = 0600;
  s2.size = 100;
  s2.mtime_how = backend::SetAttr::TimeHow::kClient;
  s2.mtime = {1234567, 0};
  s2.atime_how = backend::SetAttr::TimeHow::kServer;
  auto after = run(m.runtime, (*a3)->setattr(m.root_cred, s2));
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->mode, 0600u);
  EXPECT_EQ(after->size, 100u);
  EXPECT_EQ(after->mtime.sec, 1234567);
  EXPECT_TRUE(after->atime.sec > 1234567);
  auto dir_size = run(m.runtime, dir->obj->setattr(m.root_cred, s2));
  EXPECT_FALSE(dir_size.has_value());
  EXPECT_EQ(raw(dir_size.error()), EISDIR);
}

TEST(Cephfs, RecreatedObjectGetsNewHandle) {
  Mount m;
  auto root = m.root();
  auto f1 = run(m.runtime, root->create(m.root_cred, "f", backend::SetAttr{}, nullptr));
  ASSERT_TRUE(f1.has_value());
  auto oid1 = f1->obj->id();
  f1->obj.reset();
  m.be->flush_fd_cache();
  ASSERT_TRUE(run(m.runtime, root->unlink(m.root_cred, "f")).has_value());
  auto f2 = run(m.runtime, root->create(m.root_cred, "f", backend::SetAttr{}, nullptr));
  ASSERT_TRUE(f2.has_value());
  EXPECT_FALSE(f2->obj->id() == oid1);  // P2: inode numbers are never reused
  auto stale = run(m.runtime, m.be->resolve(oid1));
  EXPECT_FALSE(stale.has_value());
  EXPECT_EQ(raw(stale.error()), ESTALE);
}

TEST(Cephfs, ExclusiveCreateReplay) {
  Mount m;
  auto root = m.root();
  backend::ExclVerf verf{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                         std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
  auto first = run(m.runtime, root->create(m.root_cred, "x", backend::SetAttr{}, &verf));
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->attr.mode, 0u);  // EXCLUSIVE leaves mode for the follow-up SETATTR
  auto replay = run(m.runtime, root->create(m.root_cred, "x", backend::SetAttr{}, &verf));
  ASSERT_TRUE(replay.has_value());
  EXPECT_TRUE(replay->obj->id() == first->obj->id());
  backend::ExclVerf other = verf;
  other[0] = std::byte{9};
  auto conflict = run(m.runtime, root->create(m.root_cred, "x", backend::SetAttr{}, &other));
  EXPECT_FALSE(conflict.has_value());
  EXPECT_EQ(raw(conflict.error()), EEXIST);
  auto plain = run(m.runtime, root->create(m.root_cred, "x", backend::SetAttr{}, nullptr));
  EXPECT_FALSE(plain.has_value());
  EXPECT_EQ(raw(plain.error()), EEXIST);
}

TEST(Cephfs, AnonymousAndOpenStateIo) {
  Mount m;
  auto root = m.root();
  backend::SetAttr fa;
  fa.mode = 0644;
  auto f = run(m.runtime, root->create(m.root_cred, "io", fa, nullptr));
  ASSERT_TRUE(f.has_value());
  auto& obj = *f->obj;
  backend::OpenCtx anon{m.root_cred};

  auto w = run(m.runtime, obj.write(anon, 0, m.bytes("hello "), backend::Stability::kUnstable));
  ASSERT_TRUE(w.has_value());
  EXPECT_EQ(*w, 6u);
  // scatter write straddling two segments, file-sync stability
  std::string s1 = "wor", s2 = "ld!";
  iovec iov[2] = {{s1.data(), s1.size()}, {s2.data(), s2.size()}};
  auto w2 = run(m.runtime, obj.write(anon, 6, std::span<const iovec>(iov, 2),
                                     backend::Stability::kFileSync));
  ASSERT_TRUE(w2.has_value());
  EXPECT_EQ(*w2, 6u);
  EXPECT_STREQ(m.read_all(obj), "hello world!");
  // the short-write fault exercises the iovec advance
  backend::fault::arm(backend::fault::Kind::kShortWrite, 1);
  auto w_short = run(m.runtime, obj.write(anon, 12, std::span<const iovec>(iov, 2),
                                          backend::Stability::kUnstable));
  ASSERT_TRUE(w_short.has_value());
  EXPECT_EQ(*w_short, 6u);
  EXPECT_STREQ(m.read_all(obj), "hello world!world!");
  std::vector<std::byte> buf(5);
  bool eof = true;
  auto r = run(m.runtime, obj.read(anon, 0, std::span(buf), eof));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 5u);
  EXPECT_FALSE(eof);
  auto tail = run(m.runtime, obj.read(anon, 16, std::span(buf), eof));
  ASSERT_TRUE(tail.has_value());
  EXPECT_EQ(*tail, 2u);
  EXPECT_TRUE(eof);
  auto zero = run(m.runtime, obj.read(anon, 18, std::span<std::byte>{}, eof));
  ASSERT_TRUE(zero.has_value());
  EXPECT_TRUE(eof);
  EXPECT_TRUE(run(m.runtime, obj.commit(anon, 0, 0)).has_value());
  auto st = m.be->stats();
  EXPECT_TRUE(st.fd_upgrades >= 1 || st.fd_misses >= 1);

  // v4 open state: IO through the OPEN's own Fh; a read-only open state used for a
  // write falls back to the anonymous path (same-owner merge upgrade).
  backend::OpenFlags rd;
  rd.set(backend::OpenFlag::kRead);
  auto ro = run(m.runtime, obj.open(m.root_cred, rd));
  ASSERT_TRUE(ro.has_value());
  backend::OpenCtx via_open{m.root_cred, ro->get()};
  auto r2 = run(m.runtime, obj.read(via_open, 6, std::span(buf), eof));
  ASSERT_TRUE(r2.has_value());
  EXPECT_STREQ(std::string(reinterpret_cast<char*>(buf.data()), 5), "world");
  auto w3 = run(m.runtime, obj.write(via_open, 0, m.bytes("HELLO"), backend::Stability::kDataSync));
  ASSERT_TRUE(w3.has_value());
  EXPECT_STREQ(m.read_all(obj), "HELLO world!world!");
  backend::OpenFlags wr;
  wr.set(backend::OpenFlag::kRead).set(backend::OpenFlag::kWrite);
  auto rw = run(m.runtime, obj.open(m.root_cred, wr));
  ASSERT_TRUE(rw.has_value());
  backend::OpenCtx via_rw{m.root_cred, rw->get()};
  ASSERT_TRUE(run(m.runtime, obj.write(via_rw, 18, m.bytes("?"), backend::Stability::kUnstable)).has_value());
  EXPECT_TRUE(run(m.runtime, obj.commit(via_rw, 0, 0)).has_value());
  EXPECT_STREQ(m.read_all(obj), "HELLO world!world!?");
  // open() on a directory is not degraded to EOPNOTSUPP: it is a real EISDIR
  auto dopen = run(m.runtime, root->open(m.root_cred, rd));
  EXPECT_FALSE(dopen.has_value());
  EXPECT_EQ(raw(dopen.error()), EISDIR);
  // OPEN with truncate
  backend::OpenFlags trunc = wr;
  trunc.set(backend::OpenFlag::kTruncate);
  auto tr = run(m.runtime, obj.open(m.root_cred, trunc));
  ASSERT_TRUE(tr.has_value());
  auto empty = run(m.runtime, obj.getattr());
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(empty->size, 0u);
  tr->reset();

  // sticky commit poison after an injected sync failure; operator clear
  backend::fault::arm(backend::fault::Kind::kFsyncEio, 1);
  auto bad = run(m.runtime, obj.commit(anon, 0, 0));
  EXPECT_FALSE(bad.has_value());
  EXPECT_EQ(raw(bad.error()), EIO);
  auto still = run(m.runtime, obj.commit(anon, 0, 0));
  EXPECT_FALSE(still.has_value());
  EXPECT_TRUE(m.be->is_poisoned(obj.id()));
  EXPECT_EQ(m.be->clear_poison(), 1u);
  EXPECT_TRUE(run(m.runtime, obj.commit(anon, 0, 0)).has_value());
  ro->reset();
  rw->reset();
}

TEST(Cephfs, SparseOpsAndCopyRange) {
  Mount m;
  auto root = m.root();
  auto f = run(m.runtime, root->create(m.root_cred, "sp", backend::SetAttr{}, nullptr));
  ASSERT_TRUE(f.has_value());
  auto& obj = *f->obj;
  backend::OpenCtx anon{m.root_cred};
  std::string data(8192, 'x');
  ASSERT_TRUE(run(m.runtime, obj.write(anon, 0, m.bytes(data), backend::Stability::kFileSync)).has_value());
  ASSERT_TRUE(run(m.runtime, obj.allocate(anon, 8192, 4096)).has_value());
  auto grown = run(m.runtime, obj.getattr());
  ASSERT_TRUE(grown.has_value());
  EXPECT_EQ(grown->size, 12288u);
  auto hole_at_end = run(m.runtime, obj.seek(anon, 0, backend::SeekWhat::kHole));
  ASSERT_TRUE(hole_at_end.has_value());
  EXPECT_TRUE(*hole_at_end >= 8192u);
  auto data_at = run(m.runtime, obj.seek(anon, 100, backend::SeekWhat::kData));
  ASSERT_TRUE(data_at.has_value());
  EXPECT_EQ(*data_at, 100u);
  auto past = run(m.runtime, obj.seek(anon, 20000, backend::SeekWhat::kData));
  EXPECT_FALSE(past.has_value());
  EXPECT_EQ(raw(past.error()), ENXIO);
  // punch a hole in the middle; size unchanged
  auto punched = run(m.runtime, obj.deallocate(anon, 0, 4096));
  ASSERT_TRUE(punched.has_value());
  auto same = run(m.runtime, obj.getattr());
  ASSERT_TRUE(same.has_value());
  EXPECT_EQ(same->size, 12288u);
  std::vector<std::byte> buf(4);
  bool eof = false;
  auto zeros = run(m.runtime, obj.read(anon, 0, std::span(buf), eof));
  ASSERT_TRUE(zeros.has_value());
  EXPECT_EQ(static_cast<int>(buf[0]), 0);

  // copy_range is a gateway read/write loop (no copy_file_range in libcephfs)
  auto dst = run(m.runtime, root->create(m.root_cred, "cp", backend::SetAttr{}, nullptr));
  ASSERT_TRUE(dst.has_value());
  auto copied = run(m.runtime, obj.copy_range(anon, *dst->obj, anon, 4096, 0, 4096));
  ASSERT_TRUE(copied.has_value());
  EXPECT_EQ(*copied, 4096u);
  EXPECT_EQ(m.read_all(*dst->obj, 8192).size(), 4096u);
  auto to_eof = run(m.runtime, obj.copy_range(anon, *dst->obj, anon, 4096, 4096, 0));
  ASSERT_TRUE(to_eof.has_value());
  EXPECT_EQ(*to_eof, 8192u);
  auto dattr = run(m.runtime, dst->obj->getattr());
  ASSERT_TRUE(dattr.has_value());
  EXPECT_EQ(dattr->size, 12288u);
  EXPECT_STREQ(m.read_all(*dst->obj, 8192).substr(0, 8), "xxxxxxxx");
}

TEST(Cephfs, NativeChangeCounter) {
  Mount m;
  auto root = m.root();
  auto f = run(m.runtime, root->create(m.root_cred, "chg", backend::SetAttr{}, nullptr));
  ASSERT_TRUE(f.has_value());
  auto& obj = *f->obj;
  backend::OpenCtx anon{m.root_cred};
  auto a0 = run(m.runtime, obj.getattr());
  ASSERT_TRUE(a0.has_value());
  // Not the ctime synthesis: stx_version is a small counter, not nanoseconds.
  EXPECT_TRUE(a0->change < 1000000000ull);
  auto a1 = run(m.runtime, obj.getattr());
  ASSERT_TRUE(a1.has_value());
  EXPECT_EQ(a1->change, a0->change);  // nothing changed → same value
  ASSERT_TRUE(run(m.runtime, obj.write(anon, 0, m.bytes("v"), backend::Stability::kUnstable)).has_value());
  auto a2 = run(m.runtime, obj.getattr());
  ASSERT_TRUE(a2.has_value());
  EXPECT_TRUE(a2->change > a1->change);  // data change bumps it
  backend::SetAttr sa;
  sa.mode = 0600;
  auto a3 = run(m.runtime, obj.setattr(m.root_cred, sa));
  ASSERT_TRUE(a3.has_value());
  EXPECT_TRUE(a3->change > a2->change);  // metadata change bumps it
  // Directory change: creating an entry bumps the parent's counter.
  auto d0 = run(m.runtime, root->getattr());
  ASSERT_TRUE(d0.has_value());
  ASSERT_TRUE(run(m.runtime, root->mkdir(m.root_cred, "sub", backend::SetAttr{})).has_value());
  auto d1 = run(m.runtime, root->getattr());
  ASSERT_TRUE(d1.has_value());
  EXPECT_TRUE(d1->change > d0->change);
}

TEST(Cephfs, IdentityAndAccess) {
  Mount m;
  auto root = m.root();
  backend::SetAttr fa;
  fa.mode = 0600;
  auto f = run(m.runtime, root->create(m.root_cred, "priv", fa, nullptr));
  ASSERT_TRUE(f.has_value());
  uint32_t me = static_cast<uint32_t>(getuid());
  uint32_t gid = static_cast<uint32_t>(getgid());
  uint32_t other = me + 1000;
  uint32_t groups[] = {gid, 4242};
  backend::Cred owner{me, gid, groups};
  backend::Cred stranger{other, other, {}};

  // ACCESS is answered gateway-side from mode bits (one getattr, no ceph_ll_access).
  backend::AccessMask all;
  all.set(backend::Access::kRead).set(backend::Access::kModify).set(backend::Access::kExecute)
      .set(backend::Access::kLookup).set(backend::Access::kDelete).set(backend::Access::kExtend);
  uint64_t before = testing::FakeCephApi::getattr_calls();
  auto mine = run(m.runtime, f->obj->access(owner, all));
  ASSERT_TRUE(mine.has_value());
  EXPECT_TRUE(mine->has(backend::Access::kRead));
  EXPECT_TRUE(mine->has(backend::Access::kModify));
  EXPECT_FALSE(mine->has(backend::Access::kExecute));
  EXPECT_FALSE(mine->has(backend::Access::kLookup));  // not a directory
  EXPECT_EQ(testing::FakeCephApi::getattr_calls() - before, 1u);
  auto theirs = run(m.runtime, f->obj->access(stranger, all));
  ASSERT_TRUE(theirs.has_value());
  EXPECT_FALSE(theirs->has(backend::Access::kRead));

  // Every namespace call carries the caller's UserPerm to the library.
  auto as_owner = run(m.runtime, root->lookup(owner, "priv"));
  ASSERT_TRUE(as_owner.has_value());
  EXPECT_EQ(testing::FakeCephApi::last_uid(), me);
  EXPECT_EQ(testing::FakeCephApi::last_gid(), gid);
  auto as_other = run(m.runtime, root->lookup(stranger, "priv"));
  EXPECT_EQ(testing::FakeCephApi::last_uid(), other);
  (void)as_other;

  // Anonymous IO is gated by the mode bits; the owner keeps the v3 open-less
  // relaxation even when they say no (the cache handle is the gateway's).
  std::vector<std::byte> buf(4);
  bool eof = false;
  auto denied = run(m.runtime, f->obj->read({stranger}, 0, std::span(buf), eof));
  EXPECT_FALSE(denied.has_value());
  EXPECT_EQ(raw(denied.error()), EACCES);
  backend::SetAttr locked;
  locked.mode = 0000;
  ASSERT_TRUE(run(m.runtime, f->obj->setattr(owner, locked)).has_value());
  auto relaxed = run(m.runtime, f->obj->read({owner}, 0, std::span(buf), eof));
  EXPECT_TRUE(relaxed.has_value());
  // ... but the v4 OPEN is authoritative: libcephfs under the caller says EACCES,
  // no degrade to EOPNOTSUPP.
  backend::OpenFlags rd;
  rd.set(backend::OpenFlag::kRead);
  auto o = run(m.runtime, f->obj->open(owner, rd));
  EXPECT_FALSE(o.has_value());
  EXPECT_EQ(raw(o.error()), EACCES);
  EXPECT_EQ(testing::FakeCephApi::last_uid(), me);
  // A stranger cannot create in a 0700 export root: the library refuses.
  auto nope = run(m.runtime, root->create(stranger, "theirs", backend::SetAttr{}, nullptr));
  EXPECT_FALSE(nope.has_value());
  EXPECT_EQ(raw(nope.error()), EACCES);
  // No UserPerm leaks across all of the above.
  EXPECT_EQ(testing::FakeCephApi::live_perms(), 1);  // the gateway's own root perms
}

TEST(Cephfs, JukeboxAndBlocklistMapping) {
  {
    Mount m(/*jukebox=*/true);
    auto root = m.root();
    auto f = run(m.runtime, root->create(m.root_cred, "j", backend::SetAttr{}, nullptr));
    ASSERT_TRUE(f.has_value());
    std::vector<std::byte> buf(4);
    bool eof = false;
    // first IO opens the Fh (one call) — inject the failure on the read itself
    ASSERT_TRUE(run(m.runtime, f->obj->read({m.root_cred}, 0, std::span(buf), eof)).has_value());
    testing::FakeCephApi::fail_next(ENOTCONN, 1);
    auto r = run(m.runtime, f->obj->read({m.root_cred}, 0, std::span(buf), eof));
    EXPECT_FALSE(r.has_value());
    EXPECT_TRUE(r.error() == Errno::kJukebox);
    EXPECT_EQ(m.be->stats().jukebox, 1u);
    // recovered: the next IO succeeds
    EXPECT_TRUE(run(m.runtime, f->obj->read({m.root_cred}, 0, std::span(buf), eof)).has_value());
    // metadata path too (getattr → ceph_ll_getattr)
    testing::FakeCephApi::fail_next(ETIMEDOUT, 1);
    auto a = run(m.runtime, f->obj->getattr());
    EXPECT_FALSE(a.has_value());
    EXPECT_TRUE(a.error() == Errno::kJukebox);
    // the shared fault-injection kind reaches this backend as well
    backend::fault::arm(backend::fault::Kind::kJukebox, 1);
    auto fj = run(m.runtime, f->obj->read({m.root_cred}, 0, std::span(buf), eof));
    EXPECT_FALSE(fj.has_value());
    EXPECT_TRUE(fj.error() == Errno::kJukebox);
    // ordinary errors stay themselves
    testing::FakeCephApi::fail_next(ENOSPC, 1);
    auto w = run(m.runtime, f->obj->write({m.root_cred}, 0, m.bytes("x"), backend::Stability::kUnstable));
    EXPECT_FALSE(w.has_value());
    EXPECT_EQ(raw(w.error()), ENOSPC);
    // a blocklisted session is permanent: hard EIO, counted, never a retry loop
    testing::FakeCephApi::fail_next(ESHUTDOWN, 1);
    auto bl = run(m.runtime, f->obj->getattr());
    EXPECT_FALSE(bl.has_value());
    EXPECT_EQ(raw(bl.error()), EIO);
    EXPECT_EQ(m.be->stats().blocklisted, 1u);
    EXPECT_EQ(m.be->stats().jukebox, 2u);
  }
  {
    Mount m(/*jukebox=*/false);
    EXPECT_FALSE(m.be->caps().has(backend::Cap::kJukebox));
    auto root = m.root();
    testing::FakeCephApi::fail_next(ENOTCONN, 1);
    auto a = run(m.runtime, root->getattr());
    EXPECT_FALSE(a.has_value());
    EXPECT_EQ(raw(a.error()), EIO);
  }
}

TEST(Cephfs, NativeByteRangeLocks) {
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
  EXPECT_TRUE(backend::CephLockMgr::owner_key(a) != backend::CephLockMgr::owner_key(b));
  EXPECT_TRUE(backend::CephLockMgr::owner_key(a) != 0);
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
  // release closes the owner's Fh and frees everything it held
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
}

TEST(Cephfs, StopRestartAndLeakFree) {
  int inodes_before = testing::FakeCephApi::live_inodes();
  int fhs_before = testing::FakeCephApi::live_fhs();
  int perms_before = testing::FakeCephApi::live_perms();
  {
    Mount m;
    auto root = m.root();
    auto f = run(m.runtime, root->create(m.root_cred, "r", backend::SetAttr{}, nullptr));
    ASSERT_TRUE(f.has_value());
    ASSERT_TRUE(run(m.runtime, f->obj->write({m.root_cred}, 0, m.bytes("abc"), backend::Stability::kUnstable)).has_value());
    auto oid = f->obj->id();
    f->obj.reset();
    root.reset();
    ASSERT_TRUE(run(m.runtime, m.be->stop()).has_value());
    EXPECT_FALSE(m.be->started());
    auto down = run(m.runtime, m.be->root());
    EXPECT_FALSE(down.has_value());
    EXPECT_EQ(raw(down.error()), ENOTCONN);
    ASSERT_TRUE(run(m.runtime, m.be->start()).has_value());
    auto back = run(m.runtime, m.be->resolve(oid));  // inode numbers survive a remount (P1)
    ASSERT_TRUE(back.has_value());
    EXPECT_STREQ(m.read_all(**back), "abc");
    back->reset();
    // a cluster that refuses the mount fails start() cleanly
    ASSERT_TRUE(run(m.runtime, m.be->stop()).has_value());
    testing::FakeCephApi::fail_mount(ETIMEDOUT);
    auto refused = run(m.runtime, m.be->start());
    EXPECT_FALSE(refused.has_value());
    EXPECT_EQ(raw(refused.error()), ETIMEDOUT);
    testing::FakeCephApi::fail_mount(0);
  }
  EXPECT_EQ(testing::FakeCephApi::live_inodes(), inodes_before);
  EXPECT_EQ(testing::FakeCephApi::live_fhs(), fhs_before);
  EXPECT_EQ(testing::FakeCephApi::live_dirs(), 0);
  EXPECT_EQ(testing::FakeCephApi::live_perms(), perms_before);
}

TEST(Cephfs, ConfigFactory) {
  backend::register_builtin_backends();
  const auto* factory = backend::find_backend("cephfs");
  ASSERT_TRUE(factory != nullptr);
  EXPECT_TRUE(factory->virtual_path);

  backend::BackendConfig cfg;
  cfg.path = "/data";
  cfg.fsid = 5;
  cfg.values["conf"] = "/etc/ceph/ceph.conf";
  cfg.values["id"] = "lightnfs";
  cfg.values["keyring"] = "/etc/ceph/ceph.client.lightnfs.keyring";
  cfg.values["mon_host"] = "10.0.0.1,10.0.0.2";
  cfg.values["fs_name"] = "cephfs";
  cfg.values["subdir"] = "/exports/a";
  cfg.values["options"] = "client_mount_timeout=30, client_cache_size=32768";
  cfg.values["fd_cache"] = "128";
  cfg.values["readdir_enrich"] = "false";
  cfg.values["jukebox"] = "false";
  cfg.values["native_locks"] = "false";
  auto made = factory->make(cfg);
  ASSERT_TRUE(made != nullptr);
  auto* c = dynamic_cast<backend::CephBackend*>(made.get());
  ASSERT_TRUE(c != nullptr);
  EXPECT_STREQ(c->config().conf, "/etc/ceph/ceph.conf");
  EXPECT_STREQ(c->config().id, "lightnfs");
  EXPECT_STREQ(c->config().mon_host, "10.0.0.1,10.0.0.2");
  EXPECT_STREQ(c->config().fs_name, "cephfs");
  EXPECT_STREQ(c->config().subdir, "/exports/a");
  ASSERT_TRUE(c->config().options.size() == 2u);
  EXPECT_STREQ(c->config().options[0].first, "client_mount_timeout");
  EXPECT_STREQ(c->config().options[0].second, "30");
  EXPECT_STREQ(c->config().options[1].first, "client_cache_size");
  EXPECT_EQ(c->config().fd_cache, 128u);
  EXPECT_FALSE(c->config().enrich_readdir);
  EXPECT_FALSE(c->caps().has(backend::Cap::kJukebox));
  EXPECT_FALSE(c->caps().has(backend::Cap::kByteLocks));
  EXPECT_TRUE(c->caps().has(backend::Cap::kNativeChange));
  EXPECT_FALSE(c->native_locks().has_value());
  EXPECT_FALSE(c->started());

  backend::BackendConfig minimal;  // everything can come from ceph.conf
  minimal.path = "/data";
  minimal.fsid = 5;
  EXPECT_TRUE(factory->make(minimal) != nullptr);
  backend::BackendConfig bad = cfg;
  bad.values["options"] = "novalue";
  EXPECT_TRUE(factory->make(bad) == nullptr);
  backend::BackendConfig unknown = cfg;
  unknown.values["volume"] = "x";
  EXPECT_TRUE(factory->make(unknown) == nullptr);
  backend::BackendConfig relative = cfg;
  relative.values["subdir"] = "exports";
  EXPECT_TRUE(factory->make(relative) == nullptr);
  backend::BackendConfig badcache = cfg;
  badcache.values["fd_cache"] = "0";
  EXPECT_TRUE(factory->make(badcache) == nullptr);

  // Without libcephfs on this host start() reports the missing library, no crash.
  backend::CephBackend::Config plain;
  plain.fsid = 1;
  auto sys = backend::CephBackend::create(plain);
  ASSERT_TRUE(sys.has_value());
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  auto started = run(runtime, (*sys)->start());
  std::string detail;
  auto lib = backend::cephapi::load_system_api(&detail);
  if (!lib) {
    EXPECT_FALSE(started.has_value());
    EXPECT_EQ(started.error(), lib.error());
  }
  if (started) (void)run(runtime, (*sys)->stop());
  sys->reset();
  runtime.stop_and_join();
}
