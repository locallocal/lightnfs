#pragma once
// ONC RPC v2 message parsing and reply construction (design 03 §3.4; wire format per
// nfsv3 research 02-rpc-xdr). Layered error discipline:
//   header malformed / rpcvers != 2  -> MSG_DENIED(RPC_MISMATCH)
//   auth failure                     -> MSG_DENIED(AUTH_ERROR)
//   unknown prog / vers / proc       -> PROG_UNAVAIL / PROG_MISMATCH / PROC_UNAVAIL
//   arg decode failure (engine)      -> GARBAGE_ARGS
//   engine internal error            -> SYSTEM_ERR

#include <cstdint>
#include <span>

#include "xdr/xdr.hpp"

namespace lnfs::rpc {

inline constexpr uint32_t kRpcVersion = 2;
inline constexpr uint32_t kMaxAuthBody = 400;  // RFC 5531 limit

enum MsgType : uint32_t { kCall = 0, kReply = 1 };
enum ReplyStat : uint32_t { kMsgAccepted = 0, kMsgDenied = 1 };
enum AcceptStat : uint32_t {
  kSuccess = 0,
  kProgUnavail = 1,
  kProgMismatch = 2,
  kProcUnavail = 3,
  kGarbageArgs = 4,
  kSystemErr = 5,
};
enum RejectStat : uint32_t { kRpcMismatch = 0, kAuthError = 1 };
enum AuthStat : uint32_t {
  kAuthOk = 0,
  kAuthBadcred = 1,
  kAuthRejectedcred = 2,
  kAuthBadverf = 3,
  kAuthTooweak = 5,
};

struct OpaqueAuth {
  uint32_t flavor = 0;
  std::span<const std::byte> body{};
};

struct RpcCall {
  uint32_t xid = 0;
  uint32_t rpcvers = 0;
  uint32_t prog = 0;
  uint32_t vers = 0;
  uint32_t proc = 0;
  OpaqueAuth cred, verf;
  xdr::XdrDec args;  // positioned at the start of procedure arguments
};

// Parses through the auth fields; `rec` must outlive the returned RpcCall (spans reference
// it). kGarbage: not even a well-formed call header — caller drops the record.
Result<RpcCall> parse_call(const rt::BufferChain& rec);

// ---- reply headers --------------------------------------------------------
// All replies carry a NULL verf. On success the caller appends procedure results to `enc`.
void encode_reply_success(xdr::XdrEnc& enc, uint32_t xid);
void encode_reply_accepted_err(xdr::XdrEnc& enc, uint32_t xid, AcceptStat st);
void encode_reply_prog_mismatch(xdr::XdrEnc& enc, uint32_t xid, uint32_t low, uint32_t high);
void encode_reply_rpc_mismatch(xdr::XdrEnc& enc, uint32_t xid);
void encode_reply_auth_error(xdr::XdrEnc& enc, uint32_t xid, AuthStat st);

}  // namespace lnfs::rpc
