#include "rpc/dispatch.hpp"

#include "transport/connection.hpp"
#include "obs/metrics.hpp"
#include "util/log.hpp"

namespace lnfs::rpc {

using namespace lnfs::rt;

static Task<void> send_enc(transport::ConnCtx& ctx, xdr::XdrEnc& enc) {
  co_await ctx.send(enc.take());
}

Task<void> Dispatcher::reply_garbage_args(transport::ConnCtx& ctx, uint32_t xid) {
  obs::Metrics::instance().rpc_garbage.fetch_add(1, std::memory_order_relaxed);
  xdr::XdrEnc enc(ctx.pool);
  encode_reply_accepted_err(enc, xid, kGarbageArgs);
  co_await send_enc(ctx, enc);
}

Task<void> Dispatcher::handle_request(transport::ConnCtx& ctx, rt::BufferChain rec) {
  auto call = parse_call(rec);
  if (!call) {
    // Not even a parseable call header: drop the record (no xid to reply to reliably).
    LNFS_WARN("conn {}: unparseable RPC record ({} bytes), dropped", ctx.peer.to_string(),
              rec.size());
    co_return;
  }

  xdr::XdrEnc enc(ctx.pool);

  if (call->rpcvers != kRpcVersion) {
    encode_reply_rpc_mismatch(enc, call->xid);
    co_await send_enc(ctx, enc);
    co_return;
  }

  const Program* prog = nullptr;
  bool prog_known = false;
  for (const auto& p : programs_) {
    if (p.prog != call->prog) continue;
    prog_known = true;
    if (call->vers >= p.vers_lo && call->vers <= p.vers_hi) {
      prog = &p;
      break;
    }
  }
  if (!prog_known) {
    encode_reply_accepted_err(enc, call->xid, kProgUnavail);
    co_await send_enc(ctx, enc);
    co_return;
  }
  if (!prog) {
    uint32_t lo = UINT32_MAX, hi = 0;
    for (const auto& p : programs_) {
      if (p.prog != call->prog) continue;
      lo = std::min(lo, p.vers_lo);
      hi = std::max(hi, p.vers_hi);
    }
    encode_reply_prog_mismatch(enc, call->xid, lo, hi);
    co_await send_enc(ctx, enc);
    co_return;
  }

  auto cred = auth_.authenticate(*call);
  if (!cred) {
    encode_reply_auth_error(
        enc, call->xid,
        cred.error() == errno_from(EPERM) ? kAuthRejectedcred : kAuthBadcred);
    co_await send_enc(ctx, enc);
    co_return;
  }

  bool failed = false;
  try {
    co_await prog->handler(prog->self, ctx, *call, *cred);
  } catch (const std::exception& e) {
    LNFS_ERROR("conn {}: handler exception for prog={} proc={}: {}", ctx.peer.to_string(),
               call->prog, call->proc, e.what());
    failed = true;
  } catch (...) {
    LNFS_ERROR("conn {}: handler exception for prog={} proc={}", ctx.peer.to_string(),
               call->prog, call->proc);
    failed = true;
  }
  if (failed) {
    xdr::XdrEnc err(ctx.pool);
    encode_reply_accepted_err(err, call->xid, kSystemErr);
    co_await send_enc(ctx, err);
  }
}

}  // namespace lnfs::rpc
