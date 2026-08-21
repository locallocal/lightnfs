// Phase-2 write-path engine tests (development plan §4.1/§4.2): the 11 mutation
// procedures over the wire against the in-memory backend, WCC coverage on success and
// failure branches, read-only export enforcement, and DRC replay semantics.

#include "mini_test.hpp"

#include <arpa/inet.h>

#include <array>

#include "backend/memory.hpp"
#include "core/config.hpp"
#include "core/errmap.hpp"
#include "core/file_handle.hpp"
#include "core/obj_lock.hpp"
#include "nfsv3/engine.hpp"
#include "rpc/drc.hpp"
#include "runtime/reactor.hpp"
#include "runtime/testing/fake_ring.hpp"
#include "transport/connection.hpp"

using namespace lnfs;

namespace {

struct WriteFixture {
  rt::testing::FakeRing ring;
  rt::Reactor reactor{ring};
  rt::BufferPool pool;
  transport::TransportConfig transport_cfg;
  transport::ConnCtx ctx{5, peer(), pool, transport_cfg};
  core::ExportTable exports;
  backend::MemoryBackend* memory = nullptr;
  std::array<std::byte, 16> key{};
  core::FileHandleCodec handles;
  core::ObjLockRegistry locks;
  nfsv3::Engine engine;
  nfsv3::FileHandle root_fh;

  static transport::Peer peer() {
    transport::Peer p;
    auto* addr = reinterpret_cast<sockaddr_in*>(&p.addr);
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr->sin_addr);
    p.len = sizeof(*addr);
    return p;
  }

  explicit WriteFixture(bool readonly = false)
      : handles(core::FileHandleCodec::from_key(key, exports)),
        engine(exports, handles, locks) {
    auto mem = std::make_unique<backend::MemoryBackend>(23);
    memory = mem.get();
    (void)memory->add_dir("/d");
    (void)memory->add_file("/hello", "hello world");
    core::ExportConfig cfg;
    cfg.path = "/export";
    cfg.fsid = 23;
    cfg.clients = {"127.0.0.0/8"};
    cfg.readonly = readonly;
    cfg.squash = core::Squash::kNone;
    (void)exports.add(cfg, std::move(mem));
    core::WriteVerf verf{};
    verf[0] = std::byte{0xAB};
    engine.set_write_verifier(verf);
    std::optional<Result<backend::ObjPtr>> root;
    rt::spawn(
        [](backend::MemoryBackend* backend,
           std::optional<Result<backend::ObjPtr>>* out) -> rt::Task<void> {
          out->emplace(co_await backend->root());
        }(memory, &root),
        reactor);
    while (!root) reactor.poll_once();
    root_fh.data = handles.encode(*exports.by_fsid(23), (**root)->id());
  }

  rt::BufferChain call(uint32_t xid, uint32_t proc, rt::BufferChain args) {
    xdr::XdrEnc enc(pool);
    enc.u32(xid);
    enc.u32(rpc::kCall);
    enc.u32(2);
    enc.u32(nfsv3::kProgram);
    enc.u32(nfsv3::kVersion);
    enc.u32(proc);
    enc.u32(0);  // AUTH_NONE
    enc.u32(0);
    enc.u32(0);
    enc.u32(0);
    if (!args.empty()) enc.opaque_fixed(args.to_bytes());
    return enc.take();
  }

  std::vector<std::byte> request(nfsv3::Proc proc, rt::BufferChain args,
                                 uint32_t xid = 0x1234) {
    auto record = call(xid, static_cast<uint32_t>(proc), std::move(args));
    auto parsed = rpc::parse_call(record);
    if (!parsed.has_value()) return {};
    rpc::Cred root_cred;  // squash=none in the fixture: acts as root
    root_cred.uid = 0;
    root_cred.gid = 0;
    rt::spawn(engine.dispatch(ctx, *parsed, root_cred), reactor);
    while (!ring.has_pending(rt::testing::FakeRing::Kind::kSendv)) reactor.poll_once();
    auto op = ring.take(rt::testing::FakeRing::Kind::kSendv, 5);
    std::vector<std::byte> wire;
    for (int i = 0; i < op.iovcnt; ++i) {
      auto* p = static_cast<std::byte*>(op.iov[i].iov_base);
      wire.insert(wire.end(), p, p + op.iov[i].iov_len);
    }
    ring.complete(op, static_cast<int32_t>(wire.size()));
    while (reactor.poll_once()) {}
    return wire;  // includes the 4-byte record mark
  }

  static xdr::XdrDec result(std::vector<std::byte>& bytes) {
    xdr::XdrDec dec(std::span<const std::byte>(bytes.data() + 4, bytes.size() - 4));
    (void)dec.u32();  // xid
    (void)dec.u32();  // reply
    (void)dec.u32();  // accepted
    (void)dec.u32();  // verf flavor
    (void)dec.u32();  // verf len
    (void)dec.u32();  // accept_stat
    return dec;
  }

  static void skip_fattr(xdr::XdrDec& dec) {
    for (int i = 0; i < 21; ++i) (void)dec.u32();
  }
  // Returns (pre_present, post_present)
  static std::pair<bool, bool> skip_wcc(xdr::XdrDec& dec) {
    bool pre = *dec.boolean();
    if (pre) {
      (void)dec.u64();
      for (int i = 0; i < 4; ++i) (void)dec.u32();
    }
    bool post = *dec.boolean();
    if (post) skip_fattr(dec);
    return {pre, post};
  }

  rt::BufferChain enc_diropargs(const nfsv3::FileHandle& dir, std::string_view name) {
    xdr::XdrEnc enc(pool);
    nfsv3::Diropargs{dir, std::string(name)}.encode(enc);
    return enc.take();
  }

  nfsv3::FileHandle lookup_fh(const nfsv3::FileHandle& dir, std::string_view name) {
    auto reply = request(nfsv3::Proc::kLookup, enc_diropargs(dir, name));
    auto dec = result(reply);
    if (*dec.u32() != 0) return {};
    auto fh = *dec.opaque(64);
    return nfsv3::FileHandle{{fh.begin(), fh.end()}};
  }
};

}  // namespace

TEST(WriteTypes, SattrAndCreateRoundTrip) {
  rt::BufferPool pool;
  backend::SetAttr attrs;
  attrs.mode = 0640;
  attrs.size = 42;
  attrs.mtime_how = backend::SetAttr::TimeHow::kClient;
  attrs.mtime = {123, 456};
  xdr::XdrEnc enc(pool);
  nfsv3::encode_sattr(enc, attrs);
  auto chain = enc.take();
  xdr::XdrDec dec(chain);
  auto decoded = nfsv3::decode_sattr(dec);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded->mode, 0640u);
  EXPECT_EQ(*decoded->size, 42u);
  EXPECT_TRUE(decoded->mtime_how == backend::SetAttr::TimeHow::kClient);
  EXPECT_EQ(decoded->mtime.sec, 123);
  EXPECT_FALSE(decoded->uid.has_value());

  nfsv3::CreateArgs create;
  create.where = {nfsv3::FileHandle{{std::byte{9}}}, "file"};
  create.mode = nfsv3::kCreateExclusive;
  create.verf[0] = std::byte{7};
  xdr::XdrEnc enc2(pool);
  create.encode(enc2);
  auto chain2 = enc2.take();
  xdr::XdrDec dec2(chain2);
  auto out = nfsv3::CreateArgs::decode(dec2);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->mode, nfsv3::kCreateExclusive);
  EXPECT_EQ(static_cast<int>(out->verf[0]), 7);
  EXPECT_STREQ(out->where.name, "file");
}

TEST(WritePath, CreateWriteCommitReadBack) {
  WriteFixture f;
  // CREATE UNCHECKED
  nfsv3::CreateArgs create;
  create.where = {f.root_fh, "newfile"};
  create.mode = nfsv3::kCreateUnchecked;
  create.attrs.mode = 0644;
  xdr::XdrEnc enc(f.pool);
  create.encode(enc);
  auto reply = f.request(nfsv3::Proc::kCreate, enc.take());
  auto dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 0u);            // NFS3_OK
  ASSERT_TRUE(*dec.boolean());          // post_op_fh present
  auto fh_bytes = *dec.opaque(64);
  nfsv3::FileHandle file{{fh_bytes.begin(), fh_bytes.end()}};
  ASSERT_TRUE(*dec.boolean());          // post_op_attr present
  WriteFixture::skip_fattr(dec);
  auto [pre, post] = WriteFixture::skip_wcc(dec);
  EXPECT_TRUE(pre);
  EXPECT_TRUE(post);
  EXPECT_TRUE(dec.at_end());

  // WRITE UNSTABLE
  xdr::XdrEnc wenc(f.pool);
  file.encode(wenc);
  wenc.u64(0);
  wenc.u32(5);
  wenc.u32(nfsv3::kUnstable);
  const char data[] = "hello";
  wenc.opaque(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), 5));
  reply = f.request(nfsv3::Proc::kWrite, wenc.take());
  dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 0u);
  auto wcc = WriteFixture::skip_wcc(dec);
  EXPECT_TRUE(wcc.first);
  EXPECT_TRUE(wcc.second);
  EXPECT_EQ(*dec.u32(), 5u);                     // count
  EXPECT_EQ(*dec.u32(), nfsv3::kUnstable);       // committed
  auto verf = *dec.opaque_fixed(8);
  EXPECT_EQ(static_cast<int>(verf[0]), 0xAB);    // engine write verifier

  // COMMIT
  xdr::XdrEnc cenc(f.pool);
  nfsv3::CommitArgs{file, 0, 5}.encode(cenc);
  reply = f.request(nfsv3::Proc::kCommit, cenc.take());
  dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 0u);
  WriteFixture::skip_wcc(dec);
  auto cverf = *dec.opaque_fixed(8);
  EXPECT_EQ(static_cast<int>(cverf[0]), 0xAB);

  // READ back
  xdr::XdrEnc renc(f.pool);
  nfsv3::ReadArgs{file, 0, 64}.encode(renc);
  reply = f.request(nfsv3::Proc::kRead, renc.take());
  dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 0u);
  ASSERT_TRUE(*dec.boolean());
  WriteFixture::skip_fattr(dec);
  EXPECT_EQ(*dec.u32(), 5u);
  EXPECT_TRUE(*dec.boolean());  // eof
  auto got = *dec.opaque(64);
  EXPECT_STREQ(std::string(reinterpret_cast<const char*>(got.data()), got.size()),
               "hello");
}

TEST(WritePath, CreateExclusiveVerifierReplay) {
  WriteFixture f;
  nfsv3::CreateArgs create;
  create.where = {f.root_fh, "excl"};
  create.mode = nfsv3::kCreateExclusive;
  create.verf[0] = std::byte{0x11};
  auto encode = [&] {
    xdr::XdrEnc enc(f.pool);
    create.encode(enc);
    return enc.take();
  };
  auto reply = f.request(nfsv3::Proc::kCreate, encode());
  auto dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 0u);

  // Same verifier: retransmission -> success (not EEXIST).
  reply = f.request(nfsv3::Proc::kCreate, encode(), 0x9999);
  dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 0u);

  // Different verifier: genuine conflict.
  create.verf[0] = std::byte{0x22};
  reply = f.request(nfsv3::Proc::kCreate, encode(), 0x9998);
  dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 17u);  // NFS3ERR_EXIST
}

TEST(WritePath, SetattrGuardCtime) {
  WriteFixture f;
  auto file = f.lookup_fh(f.root_fh, "hello");
  // GETATTR for the current ctime
  xdr::XdrEnc genc(f.pool);
  file.encode(genc);
  auto reply = f.request(nfsv3::Proc::kGetattr, genc.take());
  auto dec = WriteFixture::result(reply);
  ASSERT_TRUE(*dec.u32() == 0u);
  for (int i = 0; i < 17; ++i) (void)dec.u32();  // to ctime (fattr words 18/19 = ctime)
  uint32_t csec = *dec.u32();
  uint32_t cnsec = *dec.u32();

  nfsv3::SetattrArgs args;
  args.object = file;
  args.attrs.mode = 0600;
  args.guard = true;
  args.guard_ctime = {static_cast<int64_t>(csec) + 100, cnsec};  // stale guard
  xdr::XdrEnc enc(f.pool);
  args.encode(enc);
  reply = f.request(nfsv3::Proc::kSetattr, enc.take());
  dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 10002u);  // NFS3ERR_NOT_SYNC
  WriteFixture::skip_wcc(dec);

  args.guard_ctime = {static_cast<int64_t>(csec), cnsec};
  xdr::XdrEnc enc2(f.pool);
  args.encode(enc2);
  reply = f.request(nfsv3::Proc::kSetattr, enc2.take());
  dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 0u);
  auto wcc = WriteFixture::skip_wcc(dec);
  EXPECT_TRUE(wcc.first);
  EXPECT_TRUE(wcc.second);
}

TEST(WritePath, RemoveRenameLinkAndFailureWcc) {
  WriteFixture f;
  auto dir = f.lookup_fh(f.root_fh, "d");

  // MKDIR + RMDIR
  nfsv3::MkdirArgs mk;
  mk.where = {f.root_fh, "sub"};
  mk.attrs.mode = 0755;
  xdr::XdrEnc enc(f.pool);
  mk.encode(enc);
  auto reply = f.request(nfsv3::Proc::kMkdir, enc.take());
  auto dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 0u);

  // RENAME sub -> sub2 (same dir; both wcc_data present)
  nfsv3::RenameArgs rn;
  rn.from = {f.root_fh, "sub"};
  rn.to = {f.root_fh, "sub2"};
  xdr::XdrEnc renc(f.pool);
  rn.encode(renc);
  reply = f.request(nfsv3::Proc::kRename, renc.take());
  dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 0u);
  auto wcc1 = WriteFixture::skip_wcc(dec);
  auto wcc2 = WriteFixture::skip_wcc(dec);
  EXPECT_TRUE(wcc1.second);
  EXPECT_TRUE(wcc2.second);
  EXPECT_TRUE(dec.at_end());

  // LINK hello into d
  auto hello = f.lookup_fh(f.root_fh, "hello");
  nfsv3::LinkArgs ln;
  ln.file = hello;
  ln.to = {dir, "hello2"};
  xdr::XdrEnc lenc(f.pool);
  ln.encode(lenc);
  reply = f.request(nfsv3::Proc::kLink, lenc.take());
  dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 0u);
  ASSERT_TRUE(*dec.boolean());  // file post-op attr
  WriteFixture::skip_fattr(dec);
  WriteFixture::skip_wcc(dec);

  // REMOVE of a missing name: failure branch still carries dir WCC post attr.
  reply = f.request(nfsv3::Proc::kRemove, f.enc_diropargs(f.root_fh, "missing"));
  dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 2u);  // NOENT
  auto wcc = WriteFixture::skip_wcc(dec);
  EXPECT_TRUE(wcc.second);

  // RMDIR sub2
  reply = f.request(nfsv3::Proc::kRmdir, f.enc_diropargs(f.root_fh, "sub2"));
  dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 0u);

  // REMOVE the link
  reply = f.request(nfsv3::Proc::kRemove, f.enc_diropargs(dir, "hello2"));
  dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 0u);
}

TEST(WritePath, ReadonlyExportRejectsWithRofs) {
  WriteFixture f(/*readonly=*/true);
  nfsv3::CreateArgs create;
  create.where = {f.root_fh, "nope"};
  create.mode = nfsv3::kCreateUnchecked;
  xdr::XdrEnc enc(f.pool);
  create.encode(enc);
  auto reply = f.request(nfsv3::Proc::kCreate, enc.take());
  auto dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 30u);  // NFS3ERR_ROFS
  auto wcc = WriteFixture::skip_wcc(dec);
  EXPECT_TRUE(wcc.second);

  auto hello = f.lookup_fh(f.root_fh, "hello");
  xdr::XdrEnc wenc(f.pool);
  hello.encode(wenc);
  wenc.u64(0);
  wenc.u32(1);
  wenc.u32(nfsv3::kFileSync);
  const char b[] = "x";
  wenc.opaque(std::span<const std::byte>(reinterpret_cast<const std::byte*>(b), 1));
  reply = f.request(nfsv3::Proc::kWrite, wenc.take());
  dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 30u);
}

TEST(WritePath, MknodRejectsBadType) {
  WriteFixture f;
  xdr::XdrEnc enc(f.pool);
  nfsv3::Diropargs{f.root_fh, "dev"}.encode(enc);
  enc.u32(1);  // NF3REG: not creatable via MKNOD
  auto reply = f.request(nfsv3::Proc::kMknod, enc.take());
  auto dec = WriteFixture::result(reply);
  EXPECT_EQ(*dec.u32(), 10007u);  // NFS3ERR_BADTYPE
}

TEST(WritePath, DrcReplaysIdenticalReply) {
  WriteFixture f;
  rpc::Drc drc({.ttl = std::chrono::milliseconds(60000), .max_memory = 1 << 20});
  f.engine.set_drc(&drc);

  nfsv3::MkdirArgs mk;
  mk.where = {f.root_fh, "drc_dir"};
  mk.attrs.mode = 0700;
  auto encode = [&] {
    xdr::XdrEnc enc(f.pool);
    mk.encode(enc);
    return enc.take();
  };
  auto first = f.request(nfsv3::Proc::kMkdir, encode(), 0x777);
  auto dec = WriteFixture::result(first);
  EXPECT_EQ(*dec.u32(), 0u);

  // Same xid + args: must replay the exact same bytes, not run MKDIR again (which
  // would yield EEXIST).
  auto second = f.request(nfsv3::Proc::kMkdir, encode(), 0x777);
  EXPECT_TRUE(first == second);
  EXPECT_EQ(drc.stats().replays, 1u);

  // Different xid: genuine new call -> EEXIST.
  auto third = f.request(nfsv3::Proc::kMkdir, encode(), 0x778);
  auto dec3 = WriteFixture::result(third);
  EXPECT_EQ(*dec3.u32(), 17u);
  EXPECT_EQ(drc.stats().inserts, 2u);
}

TEST(WritePath, ErrmapWhitelistForWriteProcs) {
  using S = nfsv3::Status;
  using P = nfsv3::Proc;
  EXPECT_EQ((uint32_t)core::to_v3(errno_from(EEXIST), P::kCreate), (uint32_t)S::kExist);
  EXPECT_EQ((uint32_t)core::to_v3(errno_from(ENOTEMPTY), P::kRmdir),
            (uint32_t)S::kNotempty);
  EXPECT_EQ((uint32_t)core::to_v3(errno_from(EXDEV), P::kRename), (uint32_t)S::kXdev);
  EXPECT_EQ((uint32_t)core::to_v3(errno_from(EMLINK), P::kLink), (uint32_t)S::kMlink);
  EXPECT_EQ((uint32_t)core::to_v3(errno_from(ENOSPC), P::kWrite), (uint32_t)S::kNospc);
  EXPECT_EQ((uint32_t)core::to_v3(errno_from(EROFS), P::kSetattr), (uint32_t)S::kRofs);
  // Out-of-whitelist mappings degrade to IO.
  EXPECT_EQ((uint32_t)core::to_v3(errno_from(ENOTEMPTY), P::kCommit), (uint32_t)S::kIo);
  EXPECT_EQ((uint32_t)core::to_v3(errno_from(EEXIST), P::kWrite), (uint32_t)S::kIo);
}
