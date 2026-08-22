// Phase-3 v4.1 engine tests (development plan §5.1–5.3): COMPOUND semantics, session
// establishment, slot replay (exactly-once), pseudo-fs crossing, read path with open
// and special stateids, response budgeting, and the errmap whitelist.

#include "mini_test.hpp"

#include <arpa/inet.h>
#include <stdlib.h>

#include <array>
#include <cstring>
#include <functional>
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

  void establish_session(bool reclaim_complete = true,
                         std::string_view owner = "lnfs-test-client") {
    xdr::XdrEnc body(pool);
    body.u32(0);  // tag len
    body.u32(1);  // minorversion
    body.u32(1);  // numops
    body.u32(static_cast<uint32_t>(Op::kExchangeId));
    std::array<std::byte, 8> verf{std::byte{9}};
    body.opaque_fixed(verf);
    body.string(owner);
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
    if (!reclaim_complete) return;
    // Like a real client: RECLAIM_COMPLETE before any non-reclaim locking op.
    xdr::XdrEnc rc(pool);
    rc.u32(static_cast<uint32_t>(Op::kReclaimComplete));
    rc.boolean(false);
    auto rr = parse(compound_raw(session_body(1, rc.take())));
    ASSERT_TRUE(rr.status == 0);
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
  uint32_t seq = f.slot_seq[0];
  auto first = f.compound_raw(make(std::nullopt));  // cachethis
  auto replay = f.compound_raw(make(seq));          // retransmit the same seq
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

// ---- phase 4: read-write + full state (development plan §6) -----------------------

namespace {

struct OpenRes {
  uint32_t status = 0;
  nfsv4::Stateid stateid{};
  std::vector<std::byte> fh;
  nfsv4::Bitmap attrset;
};

// {PUTFH dir, OPEN(...), GETFH}: parses the open result.
OpenRes do_open(V4Fixture& f, const std::vector<std::byte>& dir_fh, std::string_view name,
                uint32_t access, uint32_t deny, std::string_view owner,
                std::optional<uint32_t> create_mode = std::nullopt,
                const std::function<void(xdr::XdrEnc&)>& create_args = {},
                uint32_t claim = 0) {
  xdr::XdrEnc ops(f.pool);
  ops.u32(static_cast<uint32_t>(Op::kPutfh));
  ops.opaque(dir_fh);
  ops.u32(static_cast<uint32_t>(Op::kOpen));
  ops.u32(0);
  ops.u32(access);
  ops.u32(deny);
  ops.u64(f.clientid);
  ops.string(owner);
  if (create_mode) {
    ops.u32(1);  // OPEN4_CREATE
    ops.u32(*create_mode);
    if (create_args) create_args(ops);
  } else {
    ops.u32(0);  // OPEN4_NOCREATE
  }
  ops.u32(claim);
  if (claim == 0) ops.string(name);
  else if (claim == 1) ops.u32(0);  // delegate_type NONE
  ops.u32(static_cast<uint32_t>(Op::kGetfh));
  auto reply = f.parse(f.compound_raw(f.session_body(3, ops.take())));
  OpenRes out;
  out.status = reply.status;
  if (reply.status != 0) return out;
  V4Fixture::expect_op(reply.dec, Op::kSequence, 0);
  reply.dec.skip(16 + 5 * 4);
  V4Fixture::expect_op(reply.dec, Op::kPutfh, 0);
  V4Fixture::expect_op(reply.dec, Op::kOpen, 0);
  out.stateid = *nfsv4::Stateid::decode(reply.dec);
  (void)reply.dec.boolean();
  (void)reply.dec.u64();
  (void)reply.dec.u64();
  (void)reply.dec.u32();
  out.attrset = *nfsv4::Bitmap::decode(reply.dec);
  (void)reply.dec.u32();
  V4Fixture::expect_op(reply.dec, Op::kGetfh, 0);
  auto fh = *reply.dec.opaque(128);
  out.fh.assign(fh.begin(), fh.end());
  return out;
}

void encode_empty_fattr(xdr::XdrEnc& enc) {
  nfsv4::Bitmap none;
  none.encode(enc);
  enc.u32(0);
}

uint32_t do_write(V4Fixture& f, const std::vector<std::byte>& fh, const nfsv4::Stateid& sid,
                  uint64_t offset, std::string_view data, uint32_t stable,
                  uint32_t* count = nullptr) {
  xdr::XdrEnc ops(f.pool);
  ops.u32(static_cast<uint32_t>(Op::kPutfh));
  ops.opaque(fh);
  ops.u32(static_cast<uint32_t>(Op::kWrite));
  sid.encode(ops);
  ops.u64(offset);
  ops.u32(stable);
  ops.opaque(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()),
                                        data.size()));
  auto reply = f.parse(f.compound_raw(f.session_body(2, ops.take())));
  if (reply.status != 0) return reply.status;
  V4Fixture::expect_op(reply.dec, Op::kSequence, 0);
  reply.dec.skip(16 + 5 * 4);
  V4Fixture::expect_op(reply.dec, Op::kPutfh, 0);
  V4Fixture::expect_op(reply.dec, Op::kWrite, 0);
  uint32_t n = *reply.dec.u32();
  uint32_t committed = *reply.dec.u32();
  auto verf = *reply.dec.opaque_fixed(8);
  EXPECT_EQ(committed, stable);
  uint64_t epoch = 0;
  std::memcpy(&epoch, verf.data(), 8);
  if (count) *count = n;
  return 0;
}

std::string do_read(V4Fixture& f, const std::vector<std::byte>& fh, const nfsv4::Stateid& sid,
                    uint64_t offset, uint32_t count, uint32_t* status = nullptr) {
  xdr::XdrEnc ops(f.pool);
  ops.u32(static_cast<uint32_t>(Op::kPutfh));
  ops.opaque(fh);
  ops.u32(static_cast<uint32_t>(Op::kRead));
  sid.encode(ops);
  ops.u64(offset);
  ops.u32(count);
  auto reply = f.parse(f.compound_raw(f.session_body(2, ops.take())));
  if (status) *status = reply.status;
  if (reply.status != 0) return {};
  V4Fixture::expect_op(reply.dec, Op::kSequence, 0);
  reply.dec.skip(16 + 5 * 4);
  V4Fixture::expect_op(reply.dec, Op::kPutfh, 0);
  V4Fixture::expect_op(reply.dec, Op::kRead, 0);
  (void)reply.dec.boolean();
  auto data = *reply.dec.opaque(1 << 20);
  return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

uint32_t do_close(V4Fixture& f, const std::vector<std::byte>& fh, const nfsv4::Stateid& sid) {
  xdr::XdrEnc ops(f.pool);
  ops.u32(static_cast<uint32_t>(Op::kPutfh));
  ops.opaque(fh);
  ops.u32(static_cast<uint32_t>(Op::kClose));
  ops.u32(0);
  sid.encode(ops);
  return f.parse(f.compound_raw(f.session_body(2, ops.take()))).status;
}

// {PUTFH, GETATTR size} -> size.
uint64_t file_size(V4Fixture& f, const std::vector<std::byte>& fh) {
  xdr::XdrEnc ops(f.pool);
  ops.u32(static_cast<uint32_t>(Op::kPutfh));
  ops.opaque(fh);
  ops.u32(static_cast<uint32_t>(Op::kGetattr));
  nfsv4::Bitmap want;
  want.set(nfsv4::attr::kSize);
  want.encode(ops);
  auto reply = f.parse(f.compound_raw(f.session_body(2, ops.take())));
  if (reply.status != 0) return ~0ull;
  V4Fixture::expect_op(reply.dec, Op::kSequence, 0);
  reply.dec.skip(16 + 5 * 4);
  V4Fixture::expect_op(reply.dec, Op::kPutfh, 0);
  V4Fixture::expect_op(reply.dec, Op::kGetattr, 0);
  (void)nfsv4::Bitmap::decode(reply.dec);
  (void)reply.dec.u32();
  return *reply.dec.u64();
}

// Runs {SEQUENCE, PUTFH dir, <op encoded by body>}; returns the compound status and
// leaves `dec` positioned after the op's status word when successful.
struct OpReply {
  uint32_t status = 0;
  V4Fixture::Reply reply;
};
OpReply dir_op(V4Fixture& f, const std::vector<std::byte>& dir_fh, Op op,
               const std::function<void(xdr::XdrEnc&)>& body, uint32_t extra = 0,
               const std::function<void(xdr::XdrEnc&)>& prefix = {}) {
  xdr::XdrEnc ops(f.pool);
  if (prefix) prefix(ops);
  ops.u32(static_cast<uint32_t>(Op::kPutfh));
  ops.opaque(dir_fh);
  ops.u32(static_cast<uint32_t>(op));
  body(ops);
  OpReply out;
  out.reply = f.parse(f.compound_raw(f.session_body(2 + extra, ops.take())));
  out.status = out.reply.status;
  return out;
}

// fattr4 with mode (+ optional size) from a pool.
void fattr_mode(xdr::XdrEnc& enc, rt::BufferPool& pool, uint32_t mode,
                std::optional<uint64_t> size = {}) {
  nfsv4::Bitmap mask;
  if (size) mask.set(nfsv4::attr::kSize);
  mask.set(nfsv4::attr::kMode);
  mask.encode(enc);
  xdr::XdrEnc vals(pool);
  if (size) vals.u64(*size);
  vals.u32(mode);
  auto bytes = vals.take().to_bytes();
  enc.opaque(bytes);
}

}  // namespace

TEST(Nfs4, OpenCreateWriteCommitReadback) {
  V4Fixture f;
  f.establish_session();
  auto dir_fh = f.path_fh({"export", "data"});
  ASSERT_TRUE(!dir_fh.empty());

  // OPEN(CREATE UNCHECKED, mode 0640) for READ|WRITE.
  auto o = do_open(f, dir_fh, "newfile", 3, 0, "owner-rw", 0,
                   [&](xdr::XdrEnc& e) { fattr_mode(e, f.pool, 0640); });
  ASSERT_TRUE(o.status == 0);
  EXPECT_TRUE(o.attrset.test(nfsv4::attr::kMode));
  EXPECT_EQ(o.stateid.seqid, 1u);

  uint32_t n = 0;
  EXPECT_EQ(do_write(f, o.fh, o.stateid, 0, "hello ", 0, &n), 0u);  // UNSTABLE
  EXPECT_EQ(n, 6u);
  EXPECT_EQ(do_write(f, o.fh, o.stateid, 6, "world", 2, &n), 0u);  // FILE_SYNC
  EXPECT_EQ(n, 5u);

  // COMMIT returns the boot-epoch verifier.
  xdr::XdrEnc c(f.pool);
  c.u32(static_cast<uint32_t>(Op::kPutfh));
  c.opaque(o.fh);
  c.u32(static_cast<uint32_t>(Op::kCommit));
  c.u64(0);
  c.u32(0);
  auto cr = f.parse(f.compound_raw(f.session_body(2, c.take())));
  ASSERT_TRUE(cr.status == 0);
  V4Fixture::expect_op(cr.dec, Op::kSequence, 0);
  cr.dec.skip(16 + 5 * 4);
  V4Fixture::expect_op(cr.dec, Op::kPutfh, 0);
  V4Fixture::expect_op(cr.dec, Op::kCommit, 0);
  auto verf = *cr.dec.opaque_fixed(8);
  uint64_t epoch = 0;
  std::memcpy(&epoch, verf.data(), 8);
  EXPECT_TRUE(epoch != 0);

  EXPECT_STREQ(do_read(f, o.fh, o.stateid, 0, 64), "hello world");
  EXPECT_EQ(file_size(f, o.fh), 11u);
  // Anonymous-stateid WRITE is allowed when nothing denies it.
  nfsv4::Stateid anon{};
  EXPECT_EQ(do_write(f, o.fh, anon, 11, "!", 1, &n), 0u);
  EXPECT_STREQ(do_read(f, o.fh, anon, 0, 64), "hello world!");
  // Bad stability -> INVAL; write on a directory -> ISDIR.
  EXPECT_EQ(do_write(f, o.fh, o.stateid, 0, "x", 7), stv(Status::kInval));
  EXPECT_EQ(do_write(f, dir_fh, anon, 0, "x", 0), stv(Status::kIsdir));

  EXPECT_EQ(do_close(f, o.fh, o.stateid), 0u);
  EXPECT_EQ(f.state->stats().opens, 0u);
  // UNCHECKED re-create of an existing file with size 0 truncates.
  auto again = do_open(f, dir_fh, "newfile", 3, 0, "owner-rw", 0,
                       [&](xdr::XdrEnc& e) { fattr_mode(e, f.pool, 0640, 0); });
  ASSERT_TRUE(again.status == 0);
  EXPECT_TRUE(again.attrset.test(nfsv4::attr::kSize));
  EXPECT_EQ(file_size(f, again.fh), 0u);
  // GUARDED on an existing file -> EXIST.
  auto guarded = do_open(f, dir_fh, "newfile", 3, 0, "owner-rw", 1,
                         [&](xdr::XdrEnc& e) { fattr_mode(e, f.pool, 0640); });
  EXPECT_EQ(guarded.status, stv(Status::kExist));
  EXPECT_EQ(do_close(f, again.fh, again.stateid), 0u);
}

TEST(Nfs4, ExclusiveCreateReplayAndOpenmode) {
  V4Fixture f;
  f.establish_session();
  auto dir_fh = f.path_fh({"export", "data"});
  std::array<std::byte, 8> verf{std::byte{0xAB}};
  auto excl = [&](std::array<std::byte, 8> v, uint32_t mode) {
    return do_open(f, dir_fh, "excl", 2, 0, "owner-x", mode, [&](xdr::XdrEnc& e) {
      e.opaque_fixed(v);
      if (mode == 3) fattr_mode(e, f.pool, 0600);
    });
  };
  auto first = excl(verf, 2);  // EXCLUSIVE4
  ASSERT_TRUE(first.status == 0);
  EXPECT_TRUE(first.attrset.test(nfsv4::attr::kTimeAccess));
  auto replay = excl(verf, 2);  // same verifier: idempotent success (merged state)
  EXPECT_EQ(replay.status, 0u);
  EXPECT_EQ(replay.stateid.seqid, 2u);
  std::array<std::byte, 8> other{std::byte{0x01}};
  EXPECT_EQ(excl(other, 2).status, stv(Status::kExist));
  // EXCLUSIVE4_1 on a fresh name applies mode and reports it in attrset.
  auto e41 = do_open(f, dir_fh, "excl41", 3, 0, "owner-x", 3, [&](xdr::XdrEnc& e) {
    e.opaque_fixed(verf);
    fattr_mode(e, f.pool, 0600);
  });
  ASSERT_TRUE(e41.status == 0);
  EXPECT_TRUE(e41.attrset.test(nfsv4::attr::kMode));
  EXPECT_TRUE(e41.attrset.test(nfsv4::attr::kTimeModify));

  // READ through a WRITE-only open -> OPENMODE; WRITE through a READ-only -> OPENMODE.
  uint32_t status = 0;
  (void)do_read(f, first.fh, replay.stateid, 0, 8, &status);
  EXPECT_EQ(status, stv(Status::kOpenmode));
  auto ro = do_open(f, dir_fh, "hello", 1, 0, "owner-ro");
  ASSERT_TRUE(ro.status == 0);
  EXPECT_EQ(do_write(f, ro.fh, ro.stateid, 0, "x", 0), stv(Status::kOpenmode));
  // Write to a read-only export is ROFS via OPEN itself.
  f.exports.entries()[0]->readonly = true;
  EXPECT_EQ(do_open(f, dir_fh, "hello", 2, 0, "owner-ro").status, stv(Status::kRofs));
  f.exports.entries()[0]->readonly = false;
  EXPECT_EQ(do_close(f, ro.fh, ro.stateid), 0u);
  EXPECT_EQ(do_close(f, first.fh, first.stateid), stv(Status::kOldStateid));
  EXPECT_EQ(do_close(f, first.fh, replay.stateid), 0u);
  EXPECT_EQ(do_close(f, e41.fh, e41.stateid), 0u);
}

TEST(Nfs4, ShareDenyDowngradeAndLocked) {
  V4Fixture f;
  f.establish_session();
  auto dir_fh = f.path_fh({"export", "data"});
  auto a = do_open(f, dir_fh, "hello", 1, 2, "owner-a");  // READ, deny WRITE
  ASSERT_TRUE(a.status == 0);
  EXPECT_EQ(do_open(f, dir_fh, "hello", 2, 0, "owner-b").status, stv(Status::kShareDenied));
  EXPECT_EQ(f.state->stats().share_denied, 1u);
  // Anonymous WRITE is blocked by the deny -> LOCKED; anonymous READ passes.
  nfsv4::Stateid anon{};
  EXPECT_EQ(do_write(f, a.fh, anon, 0, "x", 0), stv(Status::kLocked));
  uint32_t status = 0;
  (void)do_read(f, a.fh, anon, 0, 4, &status);
  EXPECT_EQ(status, 0u);
  // OPEN_DOWNGRADE drops the deny; b can open for WRITE now.
  xdr::XdrEnc dg(f.pool);
  dg.u32(static_cast<uint32_t>(Op::kPutfh));
  dg.opaque(a.fh);
  dg.u32(static_cast<uint32_t>(Op::kOpenDowngrade));
  a.stateid.encode(dg);
  dg.u32(0);
  dg.u32(1);
  dg.u32(0);
  auto dr = f.parse(f.compound_raw(f.session_body(2, dg.take())));
  ASSERT_TRUE(dr.status == 0);
  V4Fixture::expect_op(dr.dec, Op::kSequence, 0);
  dr.dec.skip(16 + 5 * 4);
  V4Fixture::expect_op(dr.dec, Op::kPutfh, 0);
  V4Fixture::expect_op(dr.dec, Op::kOpenDowngrade, 0);
  auto downgraded = *nfsv4::Stateid::decode(dr.dec);
  EXPECT_EQ(downgraded.seqid, 2u);
  auto b = do_open(f, dir_fh, "hello", 2, 0, "owner-b");
  EXPECT_EQ(b.status, 0u);
  // CLAIM_FH on the file handle: same owner merges (seqid 3).
  auto fh_open = do_open(f, a.fh, "", 3, 0, "owner-a", std::nullopt, {}, 4);
  EXPECT_EQ(fh_open.status, 0u);
  EXPECT_EQ(fh_open.stateid.seqid, 3u);
  EXPECT_TRUE(fh_open.stateid.other == a.stateid.other);
  // TEST_STATEID sees both; CLOSE with the stale seqid is OLD_STATEID.
  EXPECT_EQ(do_close(f, a.fh, downgraded), stv(Status::kOldStateid));
  EXPECT_EQ(do_close(f, a.fh, fh_open.stateid), 0u);
  EXPECT_EQ(do_close(f, b.fh, b.stateid), 0u);
  EXPECT_EQ(f.state->stats().files, 0u);
}

TEST(Nfs4, SetattrSizeModeOwner) {
  V4Fixture f;
  f.establish_session();
  auto dir_fh = f.path_fh({"export", "data"});
  auto o = do_open(f, dir_fh, "hello", 3, 0, "owner-s");
  ASSERT_TRUE(o.status == 0);
  auto setattr = [&](const nfsv4::Stateid& sid, const std::function<void(xdr::XdrEnc&)>& fattr) {
    auto r = dir_op(f, o.fh, Op::kSetattr, [&](xdr::XdrEnc& e) {
      sid.encode(e);
      fattr(e);
    });
    return r;
  };
  // size=5 + mode=0600 with the open stateid.
  auto r1 = setattr(o.stateid, [&](xdr::XdrEnc& e) { fattr_mode(e, f.pool, 0600, 5); });
  ASSERT_TRUE(r1.status == 0);
  V4Fixture::expect_op(r1.reply.dec, Op::kSequence, 0);
  r1.reply.dec.skip(16 + 5 * 4);
  V4Fixture::expect_op(r1.reply.dec, Op::kPutfh, 0);
  V4Fixture::expect_op(r1.reply.dec, Op::kSetattr, 0);
  auto set = *nfsv4::Bitmap::decode(r1.reply.dec);
  EXPECT_TRUE(set.test(nfsv4::attr::kSize) && set.test(nfsv4::attr::kMode));
  EXPECT_EQ(file_size(f, o.fh), 5u);
  EXPECT_STREQ(do_read(f, o.fh, o.stateid, 0, 64), "hello");
  // Non-numeric owner -> BADOWNER (attrsset still encoded, empty).
  auto r2 = setattr(o.stateid, [&](xdr::XdrEnc& e) {
    nfsv4::Bitmap mask;
    mask.set(nfsv4::attr::kOwner);
    mask.encode(e);
    xdr::XdrEnc vals(f.pool);
    vals.string("bob@example");
    auto bytes = vals.take().to_bytes();
    e.opaque(bytes);
  });
  EXPECT_EQ(r2.status, stv(Status::kBadowner));
  // Read-only attribute in SETATTR -> INVAL; unsupported (ACL, 12) -> ATTRNOTSUPP.
  auto r3 = setattr(o.stateid, [&](xdr::XdrEnc& e) {
    nfsv4::Bitmap mask;
    mask.set(nfsv4::attr::kFileid);
    mask.encode(e);
    e.u32(0);
  });
  EXPECT_EQ(r3.status, stv(Status::kInval));
  auto r4 = setattr(o.stateid, [&](xdr::XdrEnc& e) {
    nfsv4::Bitmap mask;
    mask.set(12);
    mask.encode(e);
    e.u32(0);
  });
  EXPECT_EQ(r4.status, stv(Status::kAttrnotsupp));
  // Truncate through a foreign client's stateid -> BAD_STATEID; time_modify_set works.
  nfsv4::Stateid foreign = o.stateid;
  foreign.other[11] = std::byte{0x5A};
  auto r5 = setattr(foreign, [&](xdr::XdrEnc& e) { fattr_mode(e, f.pool, 0600, 1); });
  EXPECT_EQ(r5.status, stv(Status::kBadStateid));
  auto r6 = setattr(o.stateid, [&](xdr::XdrEnc& e) {
    nfsv4::Bitmap mask;
    mask.set(nfsv4::attr::kTimeModifySet);
    mask.encode(e);
    xdr::XdrEnc vals(f.pool);
    vals.u32(1);  // SET_TO_CLIENT_TIME4
    vals.u64(1234567);
    vals.u32(89);
    auto bytes = vals.take().to_bytes();
    e.opaque(bytes);
  });
  EXPECT_EQ(r6.status, 0u);
  EXPECT_EQ(do_close(f, o.fh, o.stateid), 0u);
}

TEST(Nfs4, NamespaceOpsCreateRemoveRenameLink) {
  V4Fixture f;
  f.establish_session();
  auto dir_fh = f.path_fh({"export", "data"});
  // CREATE dir "sub"
  auto mk = dir_op(f, dir_fh, Op::kCreate, [&](xdr::XdrEnc& e) {
    e.u32(2);  // NF4DIR
    e.string("sub");
    fattr_mode(e, f.pool, 0750);
  });
  EXPECT_EQ(mk.status, 0u);
  auto sub_fh = f.path_fh({"export", "data", "sub"});
  ASSERT_TRUE(!sub_fh.empty());
  // CREATE symlink
  auto ln = dir_op(f, dir_fh, Op::kCreate, [&](xdr::XdrEnc& e) {
    e.u32(5);  // NF4LNK
    e.string("hello");
    e.string("sym");
    encode_empty_fattr(e);
  });
  EXPECT_EQ(ln.status, 0u);
  // CREATE regular -> BADTYPE; bad name -> BADNAME
  auto reg = dir_op(f, dir_fh, Op::kCreate, [&](xdr::XdrEnc& e) {
    e.u32(1);
    e.string("r");
    encode_empty_fattr(e);
  });
  EXPECT_EQ(reg.status, stv(Status::kBadtype));
  auto badname = dir_op(f, dir_fh, Op::kCreate, [&](xdr::XdrEnc& e) {
    e.u32(2);
    e.string("a/b");
    encode_empty_fattr(e);
  });
  EXPECT_EQ(badname.status, stv(Status::kBadname));
  // LINK: SAVEFH(file) ... PUTFH(sub) LINK "hello2"
  auto hello_fh = f.path_fh({"export", "data", "hello"});
  auto link = dir_op(f, sub_fh, Op::kLink, [&](xdr::XdrEnc& e) { e.string("hello2"); }, 2,
                     [&](xdr::XdrEnc& e) {
                       e.u32(static_cast<uint32_t>(Op::kPutfh));
                       e.opaque(hello_fh);
                       e.u32(static_cast<uint32_t>(Op::kSavefh));
                     });
  EXPECT_EQ(link.status, 0u);
  EXPECT_TRUE(!f.path_fh({"export", "data", "sub", "hello2"}).empty());
  // RENAME: SAVEFH(data) ... PUTFH(sub) RENAME "hello2" -> "moved" (across dirs)
  auto ren = dir_op(f, sub_fh, Op::kRename, [&](xdr::XdrEnc& e) {
                      e.string("hello2");
                      e.string("moved");
                    }, 2, [&](xdr::XdrEnc& e) {
                      e.u32(static_cast<uint32_t>(Op::kPutfh));
                      e.opaque(sub_fh);
                      e.u32(static_cast<uint32_t>(Op::kSavefh));
                    });
  EXPECT_EQ(ren.status, 0u);
  EXPECT_TRUE(f.path_fh({"export", "data", "sub", "hello2"}).empty());
  EXPECT_TRUE(!f.path_fh({"export", "data", "sub", "moved"}).empty());
  // REMOVE non-empty dir -> NOTEMPTY; remove file then dir.
  auto rm_dir = dir_op(f, dir_fh, Op::kRemove, [&](xdr::XdrEnc& e) { e.string("sub"); });
  EXPECT_EQ(rm_dir.status, stv(Status::kNotempty));
  EXPECT_EQ(dir_op(f, sub_fh, Op::kRemove, [&](xdr::XdrEnc& e) { e.string("moved"); }).status, 0u);
  EXPECT_EQ(dir_op(f, dir_fh, Op::kRemove, [&](xdr::XdrEnc& e) { e.string("sub"); }).status, 0u);
  EXPECT_EQ(dir_op(f, dir_fh, Op::kRemove, [&](xdr::XdrEnc& e) { e.string("sub"); }).status,
            stv(Status::kNoent));
  // Pseudo root is read-only for namespace ops.
  auto root_fh = f.path_fh({});
  EXPECT_EQ(dir_op(f, root_fh, Op::kCreate, [&](xdr::XdrEnc& e) {
              e.u32(2);
              e.string("x");
              encode_empty_fattr(e);
            }).status, stv(Status::kRofs));
  EXPECT_EQ(dir_op(f, root_fh, Op::kRemove, [&](xdr::XdrEnc& e) { e.string("export"); }).status,
            stv(Status::kRofs));
}

TEST(Nfs4, RestartReclaimWithinGrace) {
  V4Fixture f;
  f.establish_session();
  auto dir_fh = f.path_fh({"export", "data"});
  auto o = do_open(f, dir_fh, "recl", 3, 0, "owner-r", 0,
                   [&](xdr::XdrEnc& e) { fattr_mode(e, f.pool, 0644); });
  ASSERT_TRUE(o.status == 0);
  EXPECT_EQ(do_write(f, o.fh, o.stateid, 0, "persist", 2), 0u);
  auto old_stateid = o.stateid;
  auto file_fh = o.fh;

  // "Restart": new epoch over the same state_dir -> grace armed from clients/.
  f.engine.reset();
  f.state.emplace(state::StateMgr::Config{.boot_epoch = 8, .state_dir = f.state_dir});
  f.state->load_grace_list();
  EXPECT_TRUE(f.state->in_grace());
  f.engine.emplace(f.exports, f.handles, f.locks, *f.pseudo, *f.state);
  f.establish_session(false);  // same co_ownerid: listed; reclaim first
  EXPECT_TRUE(f.clientid >> 32 == 8u);

  // Old stateid is STALE; plain OPEN waits in GRACE; CLAIM_PREVIOUS reclaims.
  uint32_t status = 0;
  (void)do_read(f, file_fh, old_stateid, 0, 4, &status);
  EXPECT_EQ(status, stv(Status::kStaleStateid));
  EXPECT_EQ(do_open(f, dir_fh, "recl", 3, 0, "owner-r").status, stv(Status::kGrace));
  nfsv4::Stateid anon{};
  EXPECT_EQ(do_write(f, file_fh, anon, 0, "x", 0), stv(Status::kGrace));
  auto re = do_open(f, file_fh, "", 3, 0, "owner-r", std::nullopt, {}, 1);
  ASSERT_TRUE(re.status == 0);
  EXPECT_STREQ(do_read(f, re.fh, re.stateid, 0, 64), "persist");
  EXPECT_EQ(do_write(f, re.fh, re.stateid, 7, "ed", 2), 0u);

  // RECLAIM_COMPLETE ends grace (only listed client); plain OPENs flow again.
  xdr::XdrEnc rc(f.pool);
  rc.u32(static_cast<uint32_t>(Op::kReclaimComplete));
  rc.boolean(false);
  EXPECT_EQ(f.parse(f.compound_raw(f.session_body(1, rc.take()))).status, 0u);
  EXPECT_FALSE(f.state->in_grace());
  auto after = do_open(f, dir_fh, "recl", 3, 0, "owner-r");
  EXPECT_EQ(after.status, 0u);
  EXPECT_STREQ(do_read(f, after.fh, after.stateid, 0, 64), "persisted");
  EXPECT_EQ(do_open(f, file_fh, "", 3, 0, "owner-r", std::nullopt, {}, 1).status,
            stv(Status::kNoGrace));
  EXPECT_EQ(do_close(f, after.fh, after.stateid), 0u);

  // An unlisted client reclaiming during a fresh grace is RECLAIM_BAD.
  f.engine.reset();
  f.state.emplace(state::StateMgr::Config{.boot_epoch = 9, .state_dir = f.state_dir});
  f.state->load_grace_list();
  f.engine.emplace(f.exports, f.handles, f.locks, *f.pseudo, *f.state);
  {
    xdr::XdrEnc body(f.pool);
    body.u32(0);
    body.u32(1);
    body.u32(1);
    body.u32(static_cast<uint32_t>(Op::kExchangeId));
    std::array<std::byte, 8> verf{std::byte{3}};
    body.opaque_fixed(verf);
    body.string("unlisted-client");
    body.u32(0);
    body.u32(0);
    body.u32(0);
    auto reply = f.parse(f.compound_raw(body.take()));
    ASSERT_TRUE(reply.status == 0);
    V4Fixture::expect_op(reply.dec, Op::kExchangeId, 0);
    f.clientid = *reply.dec.u64();
    uint32_t eir_seq = *reply.dec.u32();
    xdr::XdrEnc cs(f.pool);
    cs.u32(0);
    cs.u32(1);
    cs.u32(1);
    cs.u32(static_cast<uint32_t>(Op::kCreateSession));
    cs.u64(f.clientid);
    cs.u32(eir_seq);
    cs.u32(0);
    nfsv4::ChannelAttrs fore;
    fore.max_requests = 8;
    fore.encode(cs);
    fore.encode(cs);
    cs.u32(0x40000000);
    cs.u32(1);
    cs.u32(0);
    auto csr = f.parse(f.compound_raw(cs.take()));
    ASSERT_TRUE(csr.status == 0);
    V4Fixture::expect_op(csr.dec, Op::kCreateSession, 0);
    auto sid = *csr.dec.opaque_fixed(16);
    std::copy(sid.begin(), sid.end(), f.sessionid.begin());
    for (auto& s : f.slot_seq) s = 1;
  }
  EXPECT_EQ(do_open(f, file_fh, "", 3, 0, "owner-u", std::nullopt, {}, 1).status,
            stv(Status::kReclaimBad));
}

TEST(Nfs4, CurrentStateidAndReclaimCompleteGate) {
  V4Fixture f;
  f.establish_session();
  auto dir_fh = f.path_fh({"export", "data"});
  // {PUTFH dir, OPEN(CREATE), SAVEFH, PUTFH dir, RESTOREFH, WRITE(current), CLOSE(current)}
  // exercises set / save / clear / restore / consume of the current stateid in one
  // compound (RFC 8881 §16.2.3.1.2); CLOSE with seqid 0 is "current version".
  nfsv4::Stateid current{};
  current.seqid = 1;
  xdr::XdrEnc ops(f.pool);
  ops.u32(static_cast<uint32_t>(Op::kPutfh));
  ops.opaque(dir_fh);
  ops.u32(static_cast<uint32_t>(Op::kOpen));
  ops.u32(0);
  ops.u32(3 | 0x0400);  // READ|WRITE, WANT_NO_DELEG -> OPEN_DELEGATE_NONE_EXT
  ops.u32(0);
  ops.u64(f.clientid);
  ops.string("owner-cur");
  ops.u32(1);
  ops.u32(0);
  fattr_mode(ops, f.pool, 0644);
  ops.u32(0);
  ops.string("cur.txt");
  ops.u32(static_cast<uint32_t>(Op::kSavefh));
  ops.u32(static_cast<uint32_t>(Op::kPutfh));
  ops.opaque(dir_fh);
  ops.u32(static_cast<uint32_t>(Op::kRestorefh));
  ops.u32(static_cast<uint32_t>(Op::kWrite));
  current.encode(ops);
  ops.u64(0);
  ops.u32(2);
  std::string payload = "current";
  ops.opaque(std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()),
                                        payload.size()));
  ops.u32(static_cast<uint32_t>(Op::kClose));
  ops.u32(0);
  nfsv4::Stateid zero_seq = current;
  zero_seq.seqid = 1;  // still the placeholder: CLOSE consumes the current stateid
  zero_seq.encode(ops);
  auto reply = f.parse(f.compound_raw(f.session_body(7, ops.take())));
  ASSERT_TRUE(reply.status == 0);
  V4Fixture::expect_op(reply.dec, Op::kSequence, 0);
  reply.dec.skip(16 + 5 * 4);
  V4Fixture::expect_op(reply.dec, Op::kPutfh, 0);
  V4Fixture::expect_op(reply.dec, Op::kOpen, 0);
  (void)nfsv4::Stateid::decode(reply.dec);
  (void)reply.dec.boolean();
  (void)reply.dec.u64();
  (void)reply.dec.u64();
  (void)reply.dec.u32();
  (void)nfsv4::Bitmap::decode(reply.dec);
  EXPECT_EQ(*reply.dec.u32(), 3u);  // OPEN_DELEGATE_NONE_EXT
  EXPECT_EQ(*reply.dec.u32(), 0u);  // WND4_NOT_WANTED
  EXPECT_EQ(f.state->stats().opens, 0u);

  // After PUTFH the current stateid is cleared: using the placeholder -> BAD_STATEID.
  xdr::XdrEnc bad(f.pool);
  bad.u32(static_cast<uint32_t>(Op::kPutfh));
  bad.opaque(dir_fh);
  bad.u32(static_cast<uint32_t>(Op::kLookup));
  bad.string("cur.txt");
  bad.u32(static_cast<uint32_t>(Op::kRead));
  current.encode(bad);
  bad.u64(0);
  bad.u32(8);
  EXPECT_EQ(f.parse(f.compound_raw(f.session_body(3, bad.take()))).status,
            stv(Status::kBadStateid));

  // A fresh client that skips RECLAIM_COMPLETE cannot create state (§18.51.3).
  f.establish_session(false, "lnfs-late-client");
  EXPECT_EQ(do_open(f, dir_fh, "cur.txt", 3, 0, "owner-late").status, stv(Status::kGrace));
  xdr::XdrEnc rc(f.pool);
  rc.u32(static_cast<uint32_t>(Op::kReclaimComplete));
  rc.boolean(false);
  EXPECT_EQ(f.parse(f.compound_raw(f.session_body(1, rc.take()))).status, 0u);
  auto late = do_open(f, dir_fh, "cur.txt", 3, 0, "owner-late");
  EXPECT_EQ(late.status, 0u);
  EXPECT_STREQ(do_read(f, late.fh, late.stateid, 0, 16), "current");
  nfsv4::Stateid zero = late.stateid;
  zero.seqid = 0;
  EXPECT_EQ(do_close(f, late.fh, zero), 0u);  // seqid 0 accepted by CLOSE
  // Non-UTF-8 name -> INVAL.
  EXPECT_EQ(do_open(f, dir_fh, "\xC0\xC1", 1, 0, "owner-late").status, stv(Status::kInval));
}
