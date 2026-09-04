#include "mini_test.hpp"

#include <arpa/inet.h>

#include <array>

#include "backend/fault.hpp"
#include "backend/memory/memory.hpp"
#include "rpc/drc.hpp"
#include "core/config.hpp"
#include "core/errmap.hpp"
#include "core/file_handle.hpp"
#include "core/obj_lock.hpp"
#include "nfsv3/engine.hpp"
#include "mountd/mount3.hpp"
#include "runtime/reactor.hpp"
#include "runtime/testing/fake_ring.hpp"
#include "transport/connection.hpp"

using namespace lnfs;

namespace {

struct NfsFixture {
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
  mountd::Mount3 mount;
  rpc::Dispatcher dispatcher;
  nfsv3::FileHandle root_fh;

  static transport::Peer peer() {
    transport::Peer p;
    auto* addr = reinterpret_cast<sockaddr_in*>(&p.addr);
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr->sin_addr);
    p.len = sizeof(*addr);
    return p;
  }

  NfsFixture()
      : handles(core::FileHandleCodec::from_key(key, exports)),
        engine(exports, handles, locks),
        mount(exports, handles) {
    auto mem = std::make_unique<backend::MemoryBackend>(23);
    memory = mem.get();
    (void)memory->add_dir("/d");
    (void)memory->add_file("/hello", "hello world");
    (void)memory->add_file("/d/a", "a");
    (void)memory->add_symlink("/link", "hello");
    core::ExportConfig cfg;
    cfg.path = "/export";
    cfg.fsid = 23;
    cfg.clients = {"127.0.0.0/8"};
    (void)exports.add(cfg, std::move(mem));
    engine.register_with(dispatcher);
    mount.register_with(dispatcher);
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

  rt::BufferChain call(uint32_t xid, uint32_t proc, rt::BufferChain args = {},
                       uint32_t program = nfsv3::kProgram) {
    xdr::XdrEnc enc(pool);
    enc.u32(xid);
    enc.u32(rpc::kCall);
    enc.u32(2);
    enc.u32(program);
    enc.u32(nfsv3::kVersion);
    enc.u32(proc);
    enc.u32(0);  // AUTH_NONE
    enc.u32(0);
    enc.u32(0);
    enc.u32(0);
    if (!args.empty()) enc.opaque_fixed(args.to_bytes());
    return enc.take();
  }

  std::vector<std::byte> request(uint32_t proc, rt::BufferChain args = {}) {
    rt::spawn(dispatcher.handle_request(ctx, call(0x1234, proc, std::move(args))), reactor);
    while (!ring.has_pending(rt::testing::FakeRing::Kind::kSendv)) reactor.poll_once();
    auto op = ring.take(rt::testing::FakeRing::Kind::kSendv, 5);
    std::vector<std::byte> wire;
    for (int i = 0; i < op.iovcnt; ++i) {
      auto* p = static_cast<std::byte*>(op.iov[i].iov_base);
      wire.insert(wire.end(), p, p + op.iov[i].iov_len);
    }
    ring.complete(op, wire.size());
    while (reactor.poll_once()) {}
    return {wire.begin() + 4, wire.end()};
  }

  std::vector<std::byte> mount_request(uint32_t proc, rt::BufferChain args = {}) {
    rt::spawn(dispatcher.handle_request(
                  ctx, call(0x4321, proc, std::move(args), mountd::kProgram)),
              reactor);
    while (!ring.has_pending(rt::testing::FakeRing::Kind::kSendv)) reactor.poll_once();
    auto op = ring.take(rt::testing::FakeRing::Kind::kSendv, 5);
    std::vector<std::byte> wire;
    for (int i = 0; i < op.iovcnt; ++i) {
      auto* p = static_cast<std::byte*>(op.iov[i].iov_base);
      wire.insert(wire.end(), p, p + op.iov[i].iov_len);
    }
    ring.complete(op, static_cast<int32_t>(wire.size()));
    while (reactor.poll_once()) {}
    return {wire.begin() + 4, wire.end()};
  }

  xdr::XdrDec result(std::vector<std::byte>& bytes) {
    xdr::XdrDec dec(std::span<const std::byte>(bytes.data(), bytes.size()));
    (void)dec.u32();  // xid
    (void)dec.u32();  // reply
    (void)dec.u32();  // accepted
    (void)dec.u32();  // verf flavor
    (void)dec.u32();  // verf len
    (void)dec.u32();  // RPC success
    return dec;
  }
};

}  // namespace

TEST(Nfs3Types, ReadAndReaddirArgsRoundTrip) {
  rt::BufferPool pool;
  nfsv3::ReadArgs read{{{std::byte{1}, std::byte{2}}}, 99, 4096};
  xdr::XdrEnc enc(pool);
  read.encode(enc);
  auto chain = enc.take();
  xdr::XdrDec dec(chain);
  auto decoded = nfsv3::ReadArgs::decode(dec);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->offset, 99u);
  EXPECT_EQ(decoded->count, 4096u);
  EXPECT_EQ(decoded->file.data.size(), 2u);

  nfsv3::ReaddirPlusArgs plus{{{std::byte{3}}}, 7, {}, 1024, 8192};
  xdr::XdrEnc plus_enc(pool);
  plus.encode(plus_enc);
  auto plus_chain = plus_enc.take();
  xdr::XdrDec plus_dec(plus_chain);
  auto plus_out = nfsv3::ReaddirPlusArgs::decode(plus_dec);
  ASSERT_TRUE(plus_out.has_value());
  EXPECT_EQ(plus_out->cookie, 7u);
  EXPECT_EQ(plus_out->dircount, 1024u);
  EXPECT_EQ(plus_out->maxcount, 8192u);
}

TEST(Nfs3, GetattrLookupReadAndReaddirPlusWireFlow) {
  NfsFixture f;
  xdr::XdrEnc getarg(f.pool);
  f.root_fh.encode(getarg);
  auto reply = f.request((uint32_t)nfsv3::Proc::kGetattr, getarg.take());
  auto result = f.result(reply);
  EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kOk);
  EXPECT_EQ(*result.u32(), (uint32_t)backend::FType::kDir);

  xdr::XdrEnc lookarg(f.pool);
  nfsv3::Diropargs{f.root_fh, "hello"}.encode(lookarg);
  reply = f.request((uint32_t)nfsv3::Proc::kLookup, lookarg.take());
  result = f.result(reply);
  EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kOk);
  auto file_fh = *result.opaque(64);
  nfsv3::FileHandle file{{file_fh.begin(), file_fh.end()}};

  xdr::XdrEnc readarg(f.pool);
  nfsv3::ReadArgs{file, 0, 64}.encode(readarg);
  reply = f.request((uint32_t)nfsv3::Proc::kRead, readarg.take());
  result = f.result(reply);
  EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kOk);
  EXPECT_TRUE(*result.boolean());  // post-op attrs follow
  for (int i = 0; i < 21; ++i) (void)result.u32();  // fattr3 = 84 bytes
  EXPECT_EQ(*result.u32(), 11u);
  EXPECT_TRUE(*result.boolean());
  auto data = *result.opaque(64);
  EXPECT_STREQ(std::string(reinterpret_cast<const char*>(data.data()), data.size()),
               "hello world");

  xdr::XdrEnc rdarg(f.pool);
  nfsv3::ReaddirPlusArgs{f.root_fh, 0, {}, 4096, 16384}.encode(rdarg);
  reply = f.request((uint32_t)nfsv3::Proc::kReaddirplus, rdarg.take());
  result = f.result(reply);
  EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kOk);
  EXPECT_TRUE(*result.boolean());
}

TEST(Nfs3, ErrorWhitelistFiltersInvalidMappings) {
  EXPECT_EQ((uint32_t)core::to_v3(errno_from(ENOENT), nfsv3::Proc::kLookup),
            (uint32_t)nfsv3::Status::kNoent);
  EXPECT_EQ((uint32_t)core::to_v3(errno_from(ENOSPC), nfsv3::Proc::kGetattr),
            (uint32_t)nfsv3::Status::kIo);
  EXPECT_EQ((uint32_t)core::to_v3(Errno::kBadHandle, nfsv3::Proc::kRead),
            (uint32_t)nfsv3::Status::kBadhandle);
}

TEST(Nfs3, ReaddirRejectsMismatchedCookieVerifier) {
  NfsFixture f;
  std::array<std::byte, 8> verifier{};
  verifier[7] = std::byte{0xFF};  // cannot collide with a logical-clock change attr
  xdr::XdrEnc args(f.pool);
  nfsv3::ReaddirArgs{f.root_fh, 3, verifier, 4096}.encode(args);
  auto reply = f.request((uint32_t)nfsv3::Proc::kReaddir, args.take());
  auto result = f.result(reply);
  EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kBadCookie);
}

TEST(Mount3, MountAndExportWireFlow) {
  NfsFixture f;
  xdr::XdrEnc args(f.pool);
  args.string("/export");
  auto reply = f.mount_request(1, args.take());
  auto result = f.result(reply);
  EXPECT_EQ(*result.u32(), 0u);
  auto fh = result.opaque(64);
  ASSERT_TRUE(fh.has_value());
  EXPECT_TRUE(!fh->empty());
  EXPECT_EQ(*result.u32(), 1u);
  EXPECT_EQ(*result.u32(), 1u);  // AUTH_SYS

  reply = f.mount_request(5);
  result = f.result(reply);
  EXPECT_TRUE(*result.boolean());
  EXPECT_STREQ(std::string(*result.string(1024)), "/export");
}

// Development plan §9 "错误映射" row: the v3 whitelist is checked against the research
// table itself (docs/nfsv3/08-errors.md §8.2), regenerated by scripts/gen_errmap_cases.py.
// Every status the document lists for a procedure must pass v3_error_allowed, and the
// universal rows (IO/SERVERFAULT everywhere, STALE/BADHANDLE for handle-taking procs).
TEST(Nfs3, ErrorWhitelistMatchesResearchTable) {
  using nfsv3::Proc;
  using nfsv3::Status;
  struct Case {
    Proc proc;
    Status status;
    const char* proc_name;
    const char* status_name;
  };
  static const Case kCases[] = {
#include "errmap_v3_cases.inc"
  };
  for (const auto& c : kCases) {
    if (!core::v3_error_allowed(c.proc, c.status))
      MT_FAIL("doc lists %s for %s but v3_error_allowed rejects it", c.status_name,
              c.proc_name);
  }
  for (uint32_t p = 1; p <= 21; ++p) {
    auto proc = static_cast<Proc>(p);
    EXPECT_TRUE(core::v3_error_allowed(proc, Status::kIo));
    EXPECT_TRUE(core::v3_error_allowed(proc, Status::kServerfault));
    EXPECT_TRUE(core::v3_error_allowed(proc, Status::kStale));
    EXPECT_TRUE(core::v3_error_allowed(proc, Status::kBadhandle));
  }
  EXPECT_FALSE(core::v3_error_allowed(Proc::kNull, Status::kStale));
}

// Semantic cookie verifier (plan doc 10 §5.1): the reply verifier round-trips while
// the directory is unchanged; a modification invalidates the old pair (BAD_COOKIE)
// and the client restarts from cookie 0.
TEST(Nfs3, ReaddirVerifierRoundTripAndChangeDetection) {
  NfsFixture f;
  auto list = [&](uint64_t cookie, std::array<std::byte, 8> verf, uint32_t* status,
                  uint64_t* last_cookie, std::array<std::byte, 8>* out_verf) {
    xdr::XdrEnc args(f.pool);
    nfsv3::ReaddirArgs{f.root_fh, cookie, verf, 8192}.encode(args);
    auto reply = f.request((uint32_t)nfsv3::Proc::kReaddir, args.take());
    auto result = f.result(reply);
    *status = *result.u32();
    if (*status != 0) return;
    if (*result.boolean()) (void)result.skip(84);  // post_op_attr
    auto got = *result.opaque_fixed(8);
    std::copy(got.begin(), got.end(), out_verf->begin());
    while (*result.boolean()) {
      (void)result.u64();  // fileid
      (void)result.string(255);
      *last_cookie = *result.u64();
    }
  };

  uint32_t status = 1;
  uint64_t last_cookie = 0;
  std::array<std::byte, 8> verf{};
  list(0, {}, &status, &last_cookie, &verf);
  ASSERT_TRUE(status == 0);
  ASSERT_TRUE(last_cookie != 0);

  uint64_t ignore = 0;
  std::array<std::byte, 8> verf2{};
  list(last_cookie, verf, &status, &ignore, &verf2);
  EXPECT_EQ(status, 0u);  // unchanged directory: pair accepted

  ASSERT_TRUE(f.memory->add_file("/added-mid-listing", "x").has_value());
  list(last_cookie, verf, &status, &ignore, &verf2);
  EXPECT_EQ(status, (uint32_t)nfsv3::Status::kBadCookie);

  list(0, {}, &status, &last_cookie, &verf2);  // restart recovers with a fresh verifier
  EXPECT_EQ(status, 0u);
  EXPECT_FALSE(verf2 == verf);
}

// kJukebox end to end (plan doc 10 §5.3): a backend "try again later" reaches the
// wire as NFS3ERR_JUKEBOX on READ (one of the two procedures the 08 §8.2 whitelist
// admits it on; both are idempotent and never DRC-cached, so a retransmission always
// re-executes) and the retry succeeds once the backend is ready.
TEST(Nfs3, JukeboxReachesTheWireAndRetrySucceeds) {
  NfsFixture f;
  rpc::Drc drc({.ttl = std::chrono::milliseconds(60000), .max_memory = 1 << 20});
  f.engine.set_drc(&drc);
  xdr::XdrEnc lookarg(f.pool);
  nfsv3::Diropargs{f.root_fh, "hello"}.encode(lookarg);
  auto reply = f.request((uint32_t)nfsv3::Proc::kLookup, lookarg.take());
  auto result = f.result(reply);
  EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kOk);
  auto file_fh = *result.opaque(64);
  nfsv3::FileHandle file{{file_fh.begin(), file_fh.end()}};

  backend::fault::arm(backend::fault::Kind::kJukebox, 1);
  xdr::XdrEnc readarg(f.pool);
  nfsv3::ReadArgs{file, 0, 64}.encode(readarg);
  reply = f.request((uint32_t)nfsv3::Proc::kRead, readarg.take());
  result = f.result(reply);
  EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kJukebox);
  EXPECT_TRUE(*result.boolean());  // post-op attrs still follow (WCC for the retry)
  EXPECT_EQ(drc.stats().inserts, 0u);

  // Same xid, same args: re-executed, not replayed.
  xdr::XdrEnc again(f.pool);
  nfsv3::ReadArgs{file, 0, 64}.encode(again);
  reply = f.request((uint32_t)nfsv3::Proc::kRead, again.take());
  result = f.result(reply);
  EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kOk);
  EXPECT_EQ(drc.stats().replays, 0u);
  backend::fault::clear();
}
