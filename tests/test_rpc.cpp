// RPC parse + dispatch discipline (design 03 §3.4): success path, RPC_MISMATCH, AUTH_ERROR,
// PROG_UNAVAIL/PROG_MISMATCH, GARBAGE_ARGS helper, SYSTEM_ERR on handler exception,
// AUTH_SYS credential parsing.

#include "mini_test.hpp"
#include "runtime/reactor.hpp"
#include "runtime/testing/fake_ring.hpp"
#include "rpc/dispatch.hpp"
#include "transport/connection.hpp"
#include "xdr/xdr.hpp"

using namespace lnfs;
using namespace lnfs::rt;
using namespace lnfs::rpc;
using namespace lnfs::transport;
using testing::FakeRing;

namespace {

constexpr uint32_t kTestProg = 200000;

struct Fixture {
  FakeRing ring;
  Reactor r{ring};
  BufferPool pool;
  TransportConfig cfg;
  ConnCtx ctx{5, Peer{}, pool, cfg};
  Dispatcher disp;

  Fixture() {
    disp.add({kTestProg, 2, 3, nullptr,
              [](void*, ConnCtx& c, RpcCall& call, const Cred& cred) -> Task<void> {
                if (call.proc == 0) {  // NULL
                  xdr::XdrEnc enc(c.pool);
                  encode_reply_success(enc, call.xid);
                  co_await c.send(enc.take());
                  co_return;
                }
                if (call.proc == 1) {  // echo an opaque + report uid
                  auto arg = call.args.opaque(1024);
                  if (!arg) {
                    co_await Dispatcher::reply_garbage_args(c, call.xid);
                    co_return;
                  }
                  xdr::XdrEnc enc(c.pool);
                  encode_reply_success(enc, call.xid);
                  enc.u32(cred.uid);
                  enc.opaque(*arg);
                  co_await c.send(enc.take());
                  co_return;
                }
                if (call.proc == 99) throw std::runtime_error("engine bug");
                co_return;
              }});
  }

  void pump() {
    while (r.poll_once()) {
    }
  }

  void dispatch(BufferChain rec) {
    spawn(
        [](Fixture* f, BufferChain rr) -> Task<void> {
          co_await f->disp.handle_request(f->ctx, std::move(rr));
        }(this, std::move(rec)),
        r);
    pump();
  }

  // Collects the reply bytes from the pending sendv (skipping the 4B record mark).
  std::vector<std::byte> take_reply() {
    auto op = ring.take(FakeRing::Kind::kSendv, 5);
    std::vector<std::byte> out;
    for (int i = 0; i < op.iovcnt; ++i) {
      auto* p = static_cast<std::byte*>(op.iov[i].iov_base);
      out.insert(out.end(), p, p + op.iov[i].iov_len);
    }
    ring.complete(op, static_cast<int32_t>(out.size()));
    pump();
    return std::vector<std::byte>(out.begin() + 4, out.end());
  }
};

BufferChain make_call(BufferPool& pool, uint32_t xid, uint32_t prog, uint32_t vers,
                      uint32_t proc, uint32_t rpcvers = 2, uint32_t cred_flavor = 0,
                      std::span<const std::byte> cred_body = {},
                      std::span<const std::byte> args = {}) {
  xdr::XdrEnc enc(pool);
  enc.u32(xid);
  enc.u32(kCall);
  enc.u32(rpcvers);
  enc.u32(prog);
  enc.u32(vers);
  enc.u32(proc);
  enc.u32(cred_flavor);
  enc.opaque(cred_body);
  enc.u32(0);  // verf AUTH_NONE
  enc.u32(0);
  if (!args.empty()) enc.opaque_fixed(args);
  return enc.take();
}

std::vector<std::byte> auth_sys_body(BufferPool& pool, uint32_t uid, uint32_t gid,
                                     std::initializer_list<uint32_t> gids) {
  xdr::XdrEnc enc(pool);
  enc.u32(1);  // stamp
  enc.string("testhost");
  enc.u32(uid);
  enc.u32(gid);
  enc.u32(static_cast<uint32_t>(gids.size()));
  for (auto g : gids) enc.u32(g);
  return enc.take().to_bytes();
}

struct Reply {
  std::vector<std::byte> bytes;  // keeps the decoder's backing store alive
  uint32_t xid = 0, mtype = 0, stat = 0;
  xdr::XdrDec rest{std::span<const std::byte>{}};
};
Reply parse_reply(std::vector<std::byte> b) {
  Reply rp;
  rp.bytes = std::move(b);
  xdr::XdrDec dec{std::span<const std::byte>(rp.bytes.data(), rp.bytes.size())};
  rp.xid = *dec.u32();
  rp.mtype = *dec.u32();
  rp.stat = *dec.u32();
  rp.rest = std::move(dec);
  return rp;
}

}  // namespace

TEST(Rpc, NullProcSuccess) {
  Fixture f;
  f.dispatch(make_call(f.pool, 0x1001, kTestProg, 2, 0));
  auto rep = parse_reply(f.take_reply());
  EXPECT_EQ(rep.xid, 0x1001u);
  EXPECT_EQ(rep.mtype, (uint32_t)kReply);
  EXPECT_EQ(rep.stat, (uint32_t)kMsgAccepted);
  (void)rep.rest.u32();  // verf flavor
  (void)rep.rest.u32();  // verf len
  EXPECT_EQ(*rep.rest.u32(), (uint32_t)kSuccess);
}

TEST(Rpc, EchoWithAuthSys) {
  Fixture f;
  auto body = auth_sys_body(f.pool, 1000, 100, {10, 20});
  const char payload[] = "ping";
  xdr::XdrEnc argenc(f.pool);
  argenc.opaque(std::span<const std::byte>((const std::byte*)payload, 4));
  auto argbytes = argenc.take().to_bytes();
  f.dispatch(make_call(f.pool, 7, kTestProg, 2, 1, 2, 1, body, argbytes));
  auto rep = parse_reply(f.take_reply());
  EXPECT_EQ(rep.stat, (uint32_t)kMsgAccepted);
  (void)rep.rest.u32();
  (void)rep.rest.u32();  // verf
  EXPECT_EQ(*rep.rest.u32(), (uint32_t)kSuccess);
  EXPECT_EQ(*rep.rest.u32(), 1000u);  // uid seen by handler
  auto echoed = *rep.rest.opaque(64);
  EXPECT_STREQ(std::string((const char*)echoed.data(), echoed.size()), "ping");
}

TEST(Rpc, RpcVersionMismatch) {
  Fixture f;
  f.dispatch(make_call(f.pool, 8, kTestProg, 2, 0, /*rpcvers=*/3));
  auto rep = parse_reply(f.take_reply());
  EXPECT_EQ(rep.stat, (uint32_t)kMsgDenied);
  EXPECT_EQ(*rep.rest.u32(), (uint32_t)kRpcMismatch);
  EXPECT_EQ(*rep.rest.u32(), 2u);  // low
  EXPECT_EQ(*rep.rest.u32(), 2u);  // high
}

TEST(Rpc, ProgUnavail) {
  Fixture f;
  f.dispatch(make_call(f.pool, 9, 999999, 1, 0));
  auto rep = parse_reply(f.take_reply());
  EXPECT_EQ(rep.stat, (uint32_t)kMsgAccepted);
  (void)rep.rest.u32();
  (void)rep.rest.u32();  // verf
  EXPECT_EQ(*rep.rest.u32(), (uint32_t)kProgUnavail);
}

TEST(Rpc, ProgMismatchReportsRange) {
  Fixture f;
  f.dispatch(make_call(f.pool, 10, kTestProg, 7, 0));  // vers 7 not in [2,3]
  auto rep = parse_reply(f.take_reply());
  (void)rep.rest.u32();
  (void)rep.rest.u32();  // verf
  EXPECT_EQ(*rep.rest.u32(), (uint32_t)kProgMismatch);
  EXPECT_EQ(*rep.rest.u32(), 2u);
  EXPECT_EQ(*rep.rest.u32(), 3u);
}

TEST(Rpc, AuthUnknownFlavorRejected) {
  Fixture f;
  f.dispatch(make_call(f.pool, 11, kTestProg, 2, 0, 2, /*flavor=*/6));
  auto rep = parse_reply(f.take_reply());
  EXPECT_EQ(rep.stat, (uint32_t)kMsgDenied);
  EXPECT_EQ(*rep.rest.u32(), (uint32_t)kAuthError);
  EXPECT_EQ(*rep.rest.u32(), (uint32_t)kAuthRejectedcred);
}

TEST(Rpc, AuthSysMalformedBody) {
  Fixture f;
  const std::byte junk[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
  f.dispatch(make_call(f.pool, 12, kTestProg, 2, 0, 2, 1, junk));
  auto rep = parse_reply(f.take_reply());
  EXPECT_EQ(rep.stat, (uint32_t)kMsgDenied);
  EXPECT_EQ(*rep.rest.u32(), (uint32_t)kAuthError);
  EXPECT_EQ(*rep.rest.u32(), (uint32_t)kAuthBadcred);
}

TEST(Rpc, GarbageArgs) {
  Fixture f;
  // proc 1 expects an opaque; give it nothing
  f.dispatch(make_call(f.pool, 13, kTestProg, 2, 1));
  auto rep = parse_reply(f.take_reply());
  (void)rep.rest.u32();
  (void)rep.rest.u32();  // verf
  EXPECT_EQ(*rep.rest.u32(), (uint32_t)kGarbageArgs);
}

TEST(Rpc, HandlerExceptionBecomesSystemErr) {
  Fixture f;
  f.dispatch(make_call(f.pool, 14, kTestProg, 2, 99));
  auto rep = parse_reply(f.take_reply());
  (void)rep.rest.u32();
  (void)rep.rest.u32();  // verf
  EXPECT_EQ(*rep.rest.u32(), (uint32_t)kSystemErr);
}

TEST(Rpc, UnparseableRecordDropped) {
  Fixture f;
  BufferChain junk;
  auto b = f.pool.alloc(4);
  std::memcpy(b.data(), "ab", 2);
  junk.append(b, 0, 2);
  f.dispatch(std::move(junk));
  EXPECT_FALSE(f.ring.has_pending(FakeRing::Kind::kSendv));  // no reply at all
}

TEST(Rpc, ReplyRecordIsWellFormedOnWire) {
  // End-to-end sanity of the record mark around a reply.
  Fixture f;
  f.dispatch(make_call(f.pool, 21, kTestProg, 2, 0));
  auto op = f.ring.take(FakeRing::Kind::kSendv, 5);
  std::vector<std::byte> out;
  for (int i = 0; i < op.iovcnt; ++i) {
    auto* p = static_cast<std::byte*>(op.iov[i].iov_base);
    out.insert(out.end(), p, p + op.iov[i].iov_len);
  }
  f.ring.complete(op, (int32_t)out.size());
  f.pump();
  uint32_t hdr;
  std::memcpy(&hdr, out.data(), 4);
  hdr = xdr::from_be32(hdr);
  EXPECT_TRUE(hdr & 0x80000000u);                      // last fragment
  EXPECT_EQ(hdr & 0x7fffffffu, out.size() - 4);        // length matches
}
