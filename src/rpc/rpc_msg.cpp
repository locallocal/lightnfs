#include "rpc/rpc_msg.hpp"

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
