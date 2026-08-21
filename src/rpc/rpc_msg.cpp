#include "rpc/rpc_msg.hpp"

#include <algorithm>

namespace lnfs::rpc {

Result<RpcCall> parse_call(const rt::BufferChain& rec) {
  xdr::XdrDec dec(rec);
  RpcCall c{.args = xdr::XdrDec(rec)};  // placeholder; replaced below after header consumed

  c.xid = LNFS_TRY(dec.u32());
  uint32_t mtype = LNFS_TRY(dec.u32());
  if (mtype != kCall) return Err(Errno::kGarbage);
  c.rpcvers = LNFS_TRY(dec.u32());  // checked by dispatcher (needs xid for the denial)
  c.prog = LNFS_TRY(dec.u32());
  c.vers = LNFS_TRY(dec.u32());
  c.proc = LNFS_TRY(dec.u32());
  c.cred.flavor = LNFS_TRY(dec.u32());
  c.cred.body = LNFS_TRY(dec.opaque(kMaxAuthBody));
  c.verf.flavor = LNFS_TRY(dec.u32());
  c.verf.body = LNFS_TRY(dec.opaque(kMaxAuthBody));

  // FNV-1a over the first 256 bytes of the argument body, for the DRC key
  // (design 03 §3.7): a reused xid from the same peer must not replay a different call.
  {
    size_t args_off = rec.size() - dec.remaining();
    size_t want = std::min<size_t>(dec.remaining(), 256);
    uint64_t h = 1469598103934665603ull;
    size_t skipped = 0;
    for (size_t i = 0; i < rec.seg_count() && want > 0; ++i) {
      const auto& seg = rec.seg(i);
      size_t begin = 0;
      if (skipped < args_off) {
        size_t skip = std::min<size_t>(seg.len, args_off - skipped);
        skipped += skip;
        begin = skip;
        if (begin >= seg.len) continue;
      }
      size_t take = std::min<size_t>(seg.len - begin, want);
      const std::byte* p = seg.buf.data() + seg.off + begin;
      for (size_t j = 0; j < take; ++j) {
        h ^= static_cast<uint8_t>(p[j]);
        h *= 1099511628211ull;
      }
      want -= take;
    }
    c.args_hash = h;
  }

  c.args = std::move(dec);
  return c;
}

static void null_verf(xdr::XdrEnc& enc) {
  enc.u32(0);  // AUTH_NONE
  enc.u32(0);  // zero-length body
}

void encode_reply_success(xdr::XdrEnc& enc, uint32_t xid) {
  enc.u32(xid);
  enc.u32(kReply);
  enc.u32(kMsgAccepted);
  null_verf(enc);
  enc.u32(kSuccess);
}

void encode_reply_accepted_err(xdr::XdrEnc& enc, uint32_t xid, AcceptStat st) {
  enc.u32(xid);
  enc.u32(kReply);
  enc.u32(kMsgAccepted);
  null_verf(enc);
  enc.u32(st);
}

void encode_reply_prog_mismatch(xdr::XdrEnc& enc, uint32_t xid, uint32_t low, uint32_t high) {
  encode_reply_accepted_err(enc, xid, kProgMismatch);
  enc.u32(low);
  enc.u32(high);
}

void encode_reply_rpc_mismatch(xdr::XdrEnc& enc, uint32_t xid) {
  enc.u32(xid);
  enc.u32(kReply);
  enc.u32(kMsgDenied);
  enc.u32(kRpcMismatch);
  enc.u32(kRpcVersion);  // low
  enc.u32(kRpcVersion);  // high
}

void encode_reply_auth_error(xdr::XdrEnc& enc, uint32_t xid, AuthStat st) {
  enc.u32(xid);
  enc.u32(kReply);
  enc.u32(kMsgDenied);
  enc.u32(kAuthError);
  enc.u32(st);
}

}  // namespace lnfs::rpc
