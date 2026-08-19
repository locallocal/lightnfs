// Fuzz target feeding raw bytes into the full request path (design 03 §3.6, security list
// item 1): record bytes -> Dispatcher::handle_request (RPC header parse, auth parse, arg
// decode) over a FakeRing reactor. Any crash/UB is a finding; replies are drained and
// discarded.
//
// Built with -fsanitize=fuzzer,address when LNFS_BUILD_FUZZ=ON (clang); otherwise linked
// into fuzz_regress which replays corpus files (see regress_main.cpp).

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "rpc/dispatch.hpp"
#include "runtime/reactor.hpp"
#include "runtime/testing/fake_ring.hpp"
#include "transport/connection.hpp"
#include "util/log.hpp"
#include "xdr/xdr.hpp"

using namespace lnfs;
using namespace lnfs::rt;
using namespace lnfs::rpc;
using namespace lnfs::transport;

namespace {

constexpr uint32_t kFuzzProg = 300000;

// A handler that exercises the decoder shapes engines will use.
Task<void> fuzz_handler(ConnCtx& c, RpcCall& call, const Cred&) {
  auto u = call.args.u32();
  auto o = call.args.opaque(64 << 10);
  auto s = call.args.string(4096);
  auto v = call.args.u64();
  (void)u;
  (void)s;
  (void)v;
  xdr::XdrEnc enc(c.pool);
  encode_reply_success(enc, call.xid);
  if (o) enc.opaque(*o);
  co_await c.send(enc.take());
}

}  // namespace

extern "C" void lnfs_fuzz_entry(const uint8_t* data, size_t size) {
  [[maybe_unused]] static bool quiet = [] {
    lnfs::set_log_level(lnfs::LogLevel::kError);
    return true;
  }();
  testing::FakeRing ring;
  Reactor r(ring);
  BufferPool pool;
  TransportConfig cfg;
  ConnCtx ctx(3, Peer{}, pool, cfg);
  Dispatcher disp;
  disp.add({kFuzzProg, 1, 1, fuzz_handler});

  BufferChain rec;
  if (size > 0) {
    // split input across two segments to exercise chain-spanning decode paths
    auto b = pool.alloc(size);
    std::memcpy(b.data(), data, size);
    uint32_t half = static_cast<uint32_t>(size / 2);
    rec.append(b, 0, half);
    rec.append(b, half, static_cast<uint32_t>(size - half));
  }

  spawn(
      [](Dispatcher* d, ConnCtx* c, BufferChain rr) -> Task<void> {
        co_await d->handle_request(*c, std::move(rr));
      }(&disp, &ctx, std::move(rec)),
      r);

  // Drain: complete any reply sends until the reactor is idle.
  for (int guard = 0; guard < 1000; ++guard) {
    bool progress = r.poll_once();
    while (ring.has_pending(testing::FakeRing::Kind::kSendv)) {
      auto op = ring.take(testing::FakeRing::Kind::kSendv);
      size_t total = 0;
      for (int i = 0; i < op.iovcnt; ++i) total += op.iov[i].iov_len;
      ring.complete(op, static_cast<int32_t>(total));
      progress = true;
    }
    if (!progress && r.live_tasks() == 0) break;
  }
}

#ifndef LNFS_FUZZ_REGRESS
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lnfs_fuzz_entry(data, size);
  return 0;
}
#endif
