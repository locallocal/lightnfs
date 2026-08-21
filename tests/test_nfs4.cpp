// Phase-3 v4.1 engine tests (development plan §5.1–5.3): COMPOUND semantics, session
// establishment, slot replay (exactly-once), pseudo-fs crossing, read path with open
// and special stateids, response budgeting, and the errmap whitelist.

#include "mini_test.hpp"

#include <arpa/inet.h>
#include <stdlib.h>

#include <array>
#include <filesystem>
#include <set>

#include "backend/memory.hpp"
#include "core/config.hpp"
#include "core/errmap.hpp"
#include "core/file_handle.hpp"
#include "core/obj_lock.hpp"
#include "core/pseudofs.hpp"
#include "nfsv4/attrs.hpp"
#include "nfsv4/engine.hpp"
#include "runtime/reactor.hpp"
#include "runtime/testing/fake_ring.hpp"
#include "state/state_mgr.hpp"
#include "transport/connection.hpp"

using namespace lnfs;
using nfsv4::Op;
using nfsv4::Status;

namespace {

uint32_t stv(Status s) { return static_cast<uint32_t>(s); }

struct V4Fixture {
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
  std::string state_dir;
  std::optional<core::PseudoFs> pseudo;
  std::optional<state::StateMgr> state;
  std::optional<nfsv4::Engine> engine;

  static transport::Peer peer() {
    transport::Peer p;
    auto* addr = reinterpret_cast<sockaddr_in*>(&p.addr);
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr->sin_addr);
    p.len = sizeof(*addr);
    return p;
  }

  V4Fixture() : handles(core::FileHandleCodec::from_key(key, exports)) {
    auto mem = std::make_unique<backend::MemoryBackend>(23);
    memory = mem.get();
    (void)memory->add_dir("/d");
    (void)memory->add_file("/hello", "hello v4 world");
    (void)memory->add_file("/d/inner", "inner");
    (void)memory->add_symlink("/link", "hello");
    core::ExportConfig cfg;
    cfg.path = "/export/data";
    cfg.fsid = 23;
    cfg.clients = {"127.0.0.0/8"};
    cfg.squash = core::Squash::kNone;
    (void)exports.add(cfg, std::move(mem));
    char tmpl[] = "/tmp/lnfs-v4-XXXXXX";
    state_dir = mkdtemp(tmpl);
    pseudo.emplace(exports);
    state.emplace(state::StateMgr::Config{.boot_epoch = 7, .state_dir = state_dir});
    engine.emplace(exports, handles, locks, *pseudo, *state);
  }
  ~V4Fixture() {
    std::error_code ec;
    std::filesystem::remove_all(state_dir, ec);
  }

  // Sends one COMPOUND; returns the raw reply payload (past record mark).
  std::vector<std::byte> compound_raw(rt::BufferChain body, uint32_t xid = 0x44) {
    xdr::XdrEnc enc(pool);
    enc.u32(xid);
    enc.u32(rpc::kCall);
    enc.u32(2);
    enc.u32(nfsv4::kProgram);
    enc.u32(nfsv4::kVersion);
    enc.u32(1);  // COMPOUND
    enc.u32(0);
    enc.u32(0);
    enc.u32(0);
    enc.u32(0);
    enc.opaque_fixed(body.to_bytes());
    auto record = enc.take();
    auto parsed = rpc::parse_call(record);
    if (!parsed.has_value()) return {};
    rpc::Cred cred;
    cred.uid = 0;
    cred.gid = 0;
    rt::spawn(engine->dispatch(ctx, *parsed, cred), reactor);
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

  // Parses reply through the compound header; returns {status, ops_count, dec}.
  struct Reply {
    uint32_t status = 0;
    uint32_t ops = 0;
    std::vector<std::byte> bytes;
    xdr::XdrDec dec{std::span<const std::byte>{}};
  };
  Reply parse(std::vector<std::byte> payload) {
    Reply out;
    out.bytes = std::move(payload);
    out.dec = xdr::XdrDec(std::span<const std::byte>(out.bytes.data(), out.bytes.size()));
    auto& d = out.dec;
    (void)d.u32();  // xid
    (void)d.u32();  // reply
    (void)d.u32();  // accepted
    (void)d.u32();  // verf flavor
    (void)d.u32();  // verf len
    (void)d.u32();  // accept success
    out.status = *d.u32();
    (void)d.opaque(nfsv4::kMaxTag);  // tag
    out.ops = *d.u32();
    return out;
  }

  // Expects the next resop to be `op` with status `expect`; leaves dec after status.
  static void expect_op(xdr::XdrDec& d, Op op, uint32_t expect) {
    uint32_t code = *d.u32();
    ASSERT_TRUE(code == static_cast<uint32_t>(op));
    uint32_t status = *d.u32();
    ASSERT_TRUE(status == expect);
  }

  // ---- session bootstrap ----
  uint64_t clientid = 0;
  state::SessionId sessionid{};
  std::array<uint32_t, 64> slot_seq{};  // next seq per slot

  void establish_session() {
    xdr::XdrEnc body(pool);
    body.u32(0);  // tag len
    body.u32(1);  // minorversion
    body.u32(1);  // numops
    body.u32(static_cast<uint32_t>(Op::kExchangeId));
    std::array<std::byte, 8> verf{std::byte{9}};
    body.opaque_fixed(verf);
    body.string("lnfs-test-client");
    body.u32(0);  // flags
    body.u32(0);  // SP4_NONE
    body.u32(0);  // impl_id: none
    auto reply = parse(compound_raw(body.take()));
    ASSERT_TRUE(reply.status == 0);
    expect_op(reply.dec, Op::kExchangeId, 0);
    clientid = *reply.dec.u64();
    uint32_t eir_seq = *reply.dec.u32();

    xdr::XdrEnc cs(pool);
    cs.u32(0);
    cs.u32(1);
    cs.u32(1);
    cs.u32(static_cast<uint32_t>(Op::kCreateSession));
    cs.u64(clientid);
    cs.u32(eir_seq);
    cs.u32(0);  // flags
    nfsv4::ChannelAttrs fore;
    fore.max_requests = 8;
    fore.encode(cs);
    fore.encode(cs);  // back chan
    cs.u32(0x40000000);  // cb_program
    cs.u32(1);           // one sec_parms entry
    cs.u32(0);           // AUTH_NONE
    auto csr = parse(compound_raw(cs.take()));
    ASSERT_TRUE(csr.status == 0);
    expect_op(csr.dec, Op::kCreateSession, 0);
    auto sid = *csr.dec.opaque_fixed(16);
    std::copy(sid.begin(), sid.end(), sessionid.begin());
    for (auto& s : slot_seq) s = 1;
  }

  // Builds a session compound: SEQUENCE + user ops appended from `ops` encoder.
  rt::BufferChain session_body(uint32_t extra_ops, rt::BufferChain ops,
                               uint32_t slot = 0, bool cachethis = false,
                               std::optional<uint32_t> force_seq = std::nullopt) {
    xdr::XdrEnc body(pool);
    body.u32(0);  // tag
    body.u32(1);
    body.u32(1 + extra_ops);
    body.u32(static_cast<uint32_t>(Op::kSequence));
    body.opaque_fixed(sessionid);
    uint32_t seq = force_seq.value_or(slot_seq[slot]);
    body.u32(seq);
    body.u32(slot);
    body.u32(7);  // highest in use
    body.boolean(cachethis);
    if (!force_seq) slot_seq[slot]++;
    if (!ops.empty()) body.opaque_fixed(ops.to_bytes());
    return body.take();
  }

  // Runs {SEQUENCE, PUTROOTFH, LOOKUP...path, GETFH}; returns fh bytes.
  std::vector<std::byte> path_fh(std::initializer_list<std::string_view> path) {
    xdr::XdrEnc ops(pool);
    ops.u32(static_cast<uint32_t>(Op::kPutrootfh));
    for (auto name : path) {
      ops.u32(static_cast<uint32_t>(Op::kLookup));
      ops.string(name);
    }
    ops.u32(static_cast<uint32_t>(Op::kGetfh));
    auto reply = parse(compound_raw(session_body(2 + (uint32_t)path.size(), ops.take())));
    if (reply.status != 0) return {};
    expect_op(reply.dec, Op::kSequence, 0);
    reply.dec.skip(16 + 5 * 4);
    expect_op(reply.dec, Op::kPutrootfh, 0);
    for (auto name : path) {
      (void)name;
      expect_op(reply.dec, Op::kLookup, 0);
    }
    expect_op(reply.dec, Op::kGetfh, 0);
    auto fh = *reply.dec.opaque(128);
    return {fh.begin(), fh.end()};
  }
};

}  // namespace

TEST(Nfs4, MinorversionZeroRejected) {
  V4Fixture f;
  xdr::XdrEnc body(f.pool);
  body.u32(0);
  body.u32(0);  // minorversion 0
  body.u32(1);
  body.u32(static_cast<uint32_t>(Op::kPutrootfh));
  auto reply = f.parse(f.compound_raw(body.take()));
  EXPECT_EQ(reply.status, stv(Status::kMinorVersMismatch));
  EXPECT_EQ(reply.ops, 0u);
}

TEST(Nfs4, FirstOpDiscipline) {
  V4Fixture f;
  // Non-SEQUENCE, non-exempt first op
  xdr::XdrEnc body(f.pool);
  body.u32(0);
  body.u32(1);
  body.u32(1);
  body.u32(static_cast<uint32_t>(Op::kPutrootfh));
  auto reply = f.parse(f.compound_raw(body.take()));
  EXPECT_EQ(reply.status, stv(Status::kOpNotInSession));

  // Sessionless ops form solo compounds: extra ops refuse the whole compound.
  xdr::XdrEnc solo(f.pool);
  solo.u32(0);
  solo.u32(1);
  solo.u32(2);
  solo.u32(static_cast<uint32_t>(Op::kExchangeId));
  std::array<std::byte, 8> vf{};
  solo.opaque_fixed(vf);
  solo.string("solo-check");
  solo.u32(0);
  solo.u32(0);
  solo.u32(0);
  solo.u32(static_cast<uint32_t>(Op::kPutrootfh));
  auto r2 = f.parse(f.compound_raw(solo.take()));
  EXPECT_EQ(r2.status, stv(Status::kNotOnlyOp));
  EXPECT_EQ(r2.ops, 1u);
}

TEST(Nfs4, SessionEstablishAndHeartbeat) {
  V4Fixture f;
  f.establish_session();
  EXPECT_TRUE(f.clientid >> 32 == 7u);  // boot epoch in clientid

  // Empty heartbeat {SEQUENCE}
  auto reply = f.parse(f.compound_raw(f.session_body(0, {})));
  EXPECT_EQ(reply.status, 0u);
  V4Fixture::expect_op(reply.dec, Op::kSequence, 0);

  // Unknown session -> BADSESSION
  state::SessionId bogus{};
  std::swap(f.sessionid, bogus);
  auto bad = f.parse(f.compound_raw(f.session_body(0, {}, 0, false, 1)));
  std::swap(f.sessionid, bogus);
  EXPECT_EQ(bad.status, stv(Status::kBadsession));
}

TEST(Nfs4, CreateSessionReplayAndClamp) {
  V4Fixture f;
  f.establish_session();
  // Replaying CREATE_SESSION with the same sequence returns the cached body.
  xdr::XdrEnc cs(f.pool);
  cs.u32(0);
  cs.u32(1);
  cs.u32(1);
  cs.u32(static_cast<uint32_t>(Op::kCreateSession));
  cs.u64(f.clientid);
  cs.u32(1);  // the confirmed sequence again
  cs.u32(0);
  nfsv4::ChannelAttrs fore;
  fore.max_requests = 999;  // would clamp; replay must ignore and return cached
  fore.encode(cs);
  fore.encode(cs);
  cs.u32(0x40000000);
  cs.u32(1);
  cs.u32(0);
  auto reply = f.parse(f.compound_raw(cs.take()));
  EXPECT_EQ(reply.status, 0u);
  V4Fixture::expect_op(reply.dec, Op::kCreateSession, 0);
  auto sid = *reply.dec.opaque_fixed(16);
  EXPECT_TRUE(std::equal(sid.begin(), sid.end(), f.sessionid.begin()));
}

TEST(Nfs4, SlotReplayIsExactlyOnce) {
  V4Fixture f;
  f.establish_session();
  xdr::XdrEnc ops(f.pool);
  ops.u32(static_cast<uint32_t>(Op::kPutrootfh));
  ops.u32(static_cast<uint32_t>(Op::kGetfh));
  auto ops_bytes = ops.take().to_bytes();

  auto make = [&](std::optional<uint32_t> seq) {
    xdr::XdrEnc o(f.pool);
    o.opaque_fixed(ops_bytes);
    return f.session_body(2, o.take(), 0, true, seq);
  };
  auto first = f.compound_raw(make(std::nullopt));  // seq 1, cachethis
  auto replay = f.compound_raw(make(1));            // retransmit seq 1
  EXPECT_TRUE(first == replay);

  auto mis = f.parse(f.compound_raw(make(9)));  // far-future seq
  EXPECT_EQ(mis.status, stv(Status::kSeqMisordered));
}

TEST(Nfs4, PseudoFsCrossingAndAttrs) {
  V4Fixture f;
  f.establish_session();

  // Root fh is pseudo (fsid 0 in GETATTR); crossing into /export/data flips fsid to 23.
  auto root_fh = f.path_fh({});
  ASSERT_TRUE(!root_fh.empty());
  auto data_fh = f.path_fh({"export", "data"});
  ASSERT_TRUE(!data_fh.empty());

  auto getattr_fsid = [&](const std::vector<std::byte>& fh) -> uint64_t {
    xdr::XdrEnc ops(f.pool);
    ops.u32(static_cast<uint32_t>(Op::kPutfh));
    ops.opaque(fh);
    ops.u32(static_cast<uint32_t>(Op::kGetattr));
    nfsv4::Bitmap want;
    want.set(nfsv4::attr::kFsid);
    want.encode(ops);
    auto reply = f.parse(f.compound_raw(f.session_body(2, ops.take())));
    if (reply.status != 0) return ~0ull;
    V4Fixture::expect_op(reply.dec, Op::kSequence, 0);
    reply.dec.skip(16 + 5 * 4);
    V4Fixture::expect_op(reply.dec, Op::kPutfh, 0);
    V4Fixture::expect_op(reply.dec, Op::kGetattr, 0);
    auto mask = nfsv4::Bitmap::decode(reply.dec);
    (void)mask;
    (void)reply.dec.u32();  // attrlist length
    return *reply.dec.u64();  // fsid.major
  };
  EXPECT_EQ(getattr_fsid(root_fh), 0u);
  EXPECT_EQ(getattr_fsid(data_fh), 23u);

  // LOOKUP of a non-export sibling in pseudo space -> NOENT.
  xdr::XdrEnc ops(f.pool);
  ops.u32(static_cast<uint32_t>(Op::kPutrootfh));
  ops.u32(static_cast<uint32_t>(Op::kLookup));
  ops.string("nosuch");
  auto reply = f.parse(f.compound_raw(f.session_body(2, ops.take())));
  EXPECT_EQ(reply.status, stv(Status::kNoent));
}

TEST(Nfs4, OpenReadCloseAndSpecialStateids) {
  V4Fixture f;
  f.establish_session();
  auto dir_fh = f.path_fh({"export", "data"});
  ASSERT_TRUE(!dir_fh.empty());

  // OPEN(CLAIM_NULL, "hello") for read
  xdr::XdrEnc ops(f.pool);
  ops.u32(static_cast<uint32_t>(Op::kPutfh));
  ops.opaque(dir_fh);
  ops.u32(static_cast<uint32_t>(Op::kOpen));
  ops.u32(0);       // seqid
  ops.u32(1);       // share_access READ
  ops.u32(0);       // deny NONE
  ops.u64(f.clientid);
  ops.string("owner-1");
  ops.u32(0);       // OPEN4_NOCREATE
  ops.u32(0);       // CLAIM_NULL
  ops.string("hello");
  ops.u32(static_cast<uint32_t>(Op::kGetfh));
  auto reply = f.parse(f.compound_raw(f.session_body(3, ops.take())));
  ASSERT_TRUE(reply.status == 0);
  V4Fixture::expect_op(reply.dec, Op::kSequence, 0);
  reply.dec.skip(16 + 5 * 4);
  V4Fixture::expect_op(reply.dec, Op::kPutfh, 0);
  V4Fixture::expect_op(reply.dec, Op::kOpen, 0);
  auto stateid = *nfsv4::Stateid::decode(reply.dec);
  (void)reply.dec.boolean();  // change_info atomic
  (void)reply.dec.u64();
  (void)reply.dec.u64();
  (void)reply.dec.u32();  // rflags
  (void)nfsv4::Bitmap::decode(reply.dec);
  (void)reply.dec.u32();  // delegation none
  V4Fixture::expect_op(reply.dec, Op::kGetfh, 0);
  auto fhspan = *reply.dec.opaque(128);
  std::vector<std::byte> file_fh(fhspan.begin(), fhspan.end());

  auto read_with = [&](const nfsv4::Stateid& sid) {
    xdr::XdrEnc r(f.pool);
    r.u32(static_cast<uint32_t>(Op::kPutfh));
    r.opaque(file_fh);
    r.u32(static_cast<uint32_t>(Op::kRead));
    sid.encode(r);
    r.u64(0);
    r.u32(64);
    return f.parse(f.compound_raw(f.session_body(2, r.take())));
  };

  auto ok = read_with(stateid);
  ASSERT_TRUE(ok.status == 0);
  V4Fixture::expect_op(ok.dec, Op::kSequence, 0);
  ok.dec.skip(16 + 5 * 4);
  V4Fixture::expect_op(ok.dec, Op::kPutfh, 0);
  V4Fixture::expect_op(ok.dec, Op::kRead, 0);
  EXPECT_TRUE(*ok.dec.boolean());  // eof
  auto data = *ok.dec.opaque(1 << 20);
  EXPECT_STREQ(std::string(reinterpret_cast<const char*>(data.data()), data.size()),
               "hello v4 world");

  nfsv4::Stateid anon{};  // all-zero
  EXPECT_EQ(read_with(anon).status, 0u);

  nfsv4::Stateid bogus{};
  bogus.other[5] = std::byte{0x77};
  uint32_t epoch = 7;
  std::memcpy(bogus.other.data(), &epoch, 4);
  EXPECT_EQ(read_with(bogus).status, stv(Status::kBadStateid));

  nfsv4::Stateid stale{};
  epoch = 3;  // pre-restart epoch
  std::memcpy(stale.other.data(), &epoch, 4);
  stale.other[6] = std::byte{1};
  EXPECT_EQ(read_with(stale).status, stv(Status::kStaleStateid));

  // CLOSE the open state; a second CLOSE must fail with BAD_STATEID.
  auto close_with = [&](const nfsv4::Stateid& sid) {
    xdr::XdrEnc c(f.pool);
    c.u32(static_cast<uint32_t>(Op::kPutfh));
    c.opaque(file_fh);
    c.u32(static_cast<uint32_t>(Op::kClose));
    c.u32(0);
    sid.encode(c);
    return f.parse(f.compound_raw(f.session_body(2, c.take())));
  };
  EXPECT_EQ(close_with(stateid).status, 0u);
  EXPECT_EQ(close_with(stateid).status, stv(Status::kBadStateid));
}

TEST(Nfs4, ReaddirPaginatesWithinBudget) {
  V4Fixture f;
  for (int i = 0; i < 40; ++i)
    (void)f.memory->add_file("/many" + std::to_string(i), "x");
  f.establish_session();
  auto dir_fh = f.path_fh({"export", "data"});

  std::set<std::string> names;
  uint64_t cookie = 0;
  bool eof = false;
  int pages = 0;
  while (!eof && pages < 50) {
    xdr::XdrEnc ops(f.pool);
    ops.u32(static_cast<uint32_t>(Op::kPutfh));
    ops.opaque(dir_fh);
    ops.u32(static_cast<uint32_t>(Op::kReaddir));
    ops.u64(cookie);
    std::array<std::byte, 8> verf{};
    ops.opaque_fixed(verf);
    ops.u32(1u << 20);  // dircount
    ops.u32(600);       // small maxcount forces pagination
    nfsv4::Bitmap want;
    want.set(nfsv4::attr::kFileid);
    want.encode(ops);
    auto reply = f.parse(f.compound_raw(f.session_body(2, ops.take())));
    ASSERT_TRUE(reply.status == 0);
    V4Fixture::expect_op(reply.dec, Op::kSequence, 0);
    reply.dec.skip(16 + 5 * 4);
    V4Fixture::expect_op(reply.dec, Op::kPutfh, 0);
    V4Fixture::expect_op(reply.dec, Op::kReaddir, 0);
    (void)reply.dec.opaque_fixed(8);
    while (*reply.dec.boolean()) {
      cookie = *reply.dec.u64();
      auto name = *reply.dec.string(255);
      ASSERT_TRUE(names.insert(std::string(name)).second);  // no duplicates
      (void)nfsv4::Bitmap::decode(reply.dec);
      auto vals = *reply.dec.u32();
      (void)reply.dec.skip((vals + 3) & ~3u);
    }
    eof = *reply.dec.boolean();
    ++pages;
  }
  EXPECT_TRUE(eof);
  EXPECT_TRUE(pages > 1);         // budget actually paginated
  EXPECT_EQ(names.size(), 43u);  // many0..39 + hello + d + link (inner nested)
}

TEST(Nfs4, ErrmapV4Whitelist) {
  using core::to_v4;
  EXPECT_EQ((uint32_t)to_v4(errno_from(ENOENT), Op::kLookup), stv(Status::kNoent));
  EXPECT_EQ((uint32_t)to_v4(errno_from(ENOTDIR), Op::kLookup), stv(Status::kNotdir));
  EXPECT_EQ((uint32_t)to_v4(errno_from(EISDIR), Op::kRead), stv(Status::kIsdir));
  EXPECT_EQ((uint32_t)to_v4(Errno::kJukebox, Op::kRead), stv(Status::kDelay));
  // Out-of-whitelist degrades to IO.
  EXPECT_EQ((uint32_t)to_v4(errno_from(ENOTEMPTY), Op::kGetattr), stv(Status::kIo));
}
