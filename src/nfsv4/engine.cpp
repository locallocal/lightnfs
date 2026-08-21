#include "nfsv4/engine.hpp"

#include <algorithm>
#include <cstring>

#include "core/errmap.hpp"
#include "transport/connection.hpp"
#include "core/readdir.hpp"
#include "nfsv4/attrs.hpp"
#include "obs/metrics.hpp"
#include "util/log.hpp"

namespace lnfs::nfsv4 {
namespace {

using rpc::RpcCall;
using transport::ConnCtx;

uint32_t st(Status s) { return static_cast<uint32_t>(s); }

uint64_t conn_id_of(ConnCtx& conn) { return reinterpret_cast<uint64_t>(&conn); }

// Client-settable EXCHANGE_ID flags (RFC 8881 §18.35).
constexpr uint32_t kEidUpdate = 0x40000000;      // UPD_CONFIRMED_REC_A
constexpr uint32_t kEidConfirmedR = 0x80000000;  // reply-only
constexpr uint32_t kEidValidRequest = 0x00000103 | 0x00070000 | kEidUpdate;

void patch_u32(std::byte* gap, uint32_t value) {
  uint32_t be = xdr::to_be32(value);
  std::memcpy(gap, &be, 4);
}

// v4 ACCESS masks share the v3 bit values.
constexpr uint32_t kAccessRead = 0x01, kAccessLookup = 0x02, kAccessModify = 0x04,
                   kAccessExtend = 0x08, kAccessDelete = 0x10, kAccessExecute = 0x20;

bool valid_component4(std::string_view name) {
  return !name.empty() && name.size() <= kMaxName &&
         name.find('/') == std::string_view::npos &&
         name.find('\0') == std::string_view::npos && name != "." && name != "..";
}

bool valid_utf8(std::span<const std::byte> bytes) {
  size_t i = 0, n = bytes.size();
  while (i < n) {
    uint8_t c = static_cast<uint8_t>(bytes[i]);
    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3
                 : (c >> 3) == 0x1E ? 4 : 0;
    if (len == 0 || i + len > n) return false;
    for (size_t k = 1; k < len; ++k)
      if ((static_cast<uint8_t>(bytes[i + k]) & 0xC0) != 0x80) return false;
    if (len == 2 && c < 0xC2) return false;                    // overlong
    if (len == 3 && c == 0xE0 && static_cast<uint8_t>(bytes[i + 1]) < 0xA0) return false;
    if (len == 3 && c == 0xED && static_cast<uint8_t>(bytes[i + 1]) > 0x9F)
      return false;  // UTF-16 surrogates
    if (len == 3 && c == 0xEF && static_cast<uint8_t>(bytes[i + 1]) == 0xBF &&
        (static_cast<uint8_t>(bytes[i + 2]) == 0xBE ||
         static_cast<uint8_t>(bytes[i + 2]) == 0xBF))
      return false;  // U+FFFE / U+FFFF
    if (len == 4 && c == 0xF0 && static_cast<uint8_t>(bytes[i + 1]) < 0x90) return false;
    if (len == 4 && (c > 0xF4 || (c == 0xF4 && static_cast<uint8_t>(bytes[i + 1]) > 0x8F)))
      return false;
    i += len;
  }
  return true;
}

bool sessionless_op(uint32_t op) {
  switch (static_cast<Op>(op)) {
    case Op::kExchangeId:
    case Op::kCreateSession:
    case Op::kDestroySession:
    case Op::kDestroyClientid:
    case Op::kBindConnToSession: return true;
    default: return false;
  }
}

}  // namespace

void Engine::register_with(rpc::Dispatcher& dispatcher) {
  dispatcher.add({kProgram, kVersion, kVersion,
                  [this](ConnCtx& ctx, RpcCall& call, const rpc::Cred& cred) {
                    return dispatch(ctx, call, cred);
                  }});
}

rt::Task<void> Engine::dispatch(ConnCtx& ctx, RpcCall& call, const rpc::Cred& cred) {
  if (call.proc == 0) {  // NULL
    xdr::XdrEnc enc(ctx.pool);
    rpc::encode_reply_success(enc, call.xid);
    co_await ctx.send(enc.take());
    co_return;
  }
  if (call.proc != 1) {  // only NULL and COMPOUND exist
    xdr::XdrEnc enc(ctx.pool);
    rpc::encode_reply_accepted_err(enc, call.xid, rpc::kProcUnavail);
    co_await ctx.send(enc.take());
    co_return;
  }
  co_await compound(ctx, call, cred);
}

Engine::FhBytes Engine::pseudo_fh(const core::PseudoFs::Node& node) const {
  return handles_.encode_raw(0, core::PseudoFs::oid_of(node));
}

Engine::FhBytes Engine::export_fh(const core::ExportEntry& exp,
                                  const backend::ObjId& oid) const {
  return handles_.encode(exp, oid);
}

rt::Task<Result<Engine::Resolved>> Engine::resolve(const FhBytes& fh,
                                                   const sockaddr_storage& peer) {
  auto decoded = handles_.decode_v4(fh, peer);
  if (!decoded) co_return Err(decoded.error());
  Resolved out;
  if (decoded->fsid == 0) {
    out.node = pseudo_.resolve(decoded->oid);
    if (!out.node) co_return Err(errno_from(ESTALE));
    out.oid = decoded->oid;
    co_return out;
  }
  out.exp = decoded->exp;
  auto obj = co_await out.exp->backend->resolve(decoded->oid);
  if (!obj) co_return Err(obj.error());
  out.obj = std::move(*obj);
  out.oid = decoded->oid;
  co_return out;
}

// ---- compound driver -------------------------------------------------------

rt::Task<void> Engine::compound(ConnCtx& conn, RpcCall& call, const rpc::Cred& cred) {
  auto& dec = call.args;
  size_t request_size = dec.remaining() + 44;  // + RPC call header approximation
  auto tag = dec.opaque(kMaxTag);
  auto minor = dec.u32();
  auto numops = dec.u32();
  if (!tag || !minor || !numops) {
    co_await rpc::Dispatcher::reply_garbage_args(conn, call.xid);
    co_return;
  }
  std::vector<std::byte> tag_bytes(tag->begin(), tag->end());
  bool tag_ok = valid_utf8(tag_bytes);

  xdr::XdrEnc enc(conn.pool);
  rpc::encode_reply_success(enc, call.xid);
  std::byte* status_gap = enc.raw_gap(4);
  enc.opaque(tag_bytes);
  std::byte* count_gap = enc.raw_gap(4);

  auto finish = [&](uint32_t status, uint32_t ops_done) -> rt::Task<void> {
    patch_u32(status_gap, status);
    patch_u32(count_gap, ops_done);
    co_await conn.send(enc.take());
  };

  if (*minor != 1) {  // decision D5: minorversion 0 (and 2, for now) rejected
    co_await finish(st(Status::kMinorVersMismatch), 0);
    co_return;
  }
  if (!tag_ok) {  // utf8str_cs discipline (RFC 8881 §16.2)
    co_await finish(st(Status::kInval), 0);
    co_return;
  }
  if (*numops == 0) {
    co_await finish(st(Status::kOk), 0);
    co_return;
  }

  auto first_op = dec.u32();
  if (!first_op) {
    co_await rpc::Dispatcher::reply_garbage_args(conn, call.xid);
    co_return;
  }

  Ctx ctx{.conn = conn, .cred = cred};
  uint32_t status;
  uint32_t done = 0;

  if (static_cast<Op>(*first_op) == Op::kSequence) {
    // SEQUENCE args
    auto sid = dec.opaque_fixed(16);
    auto sq = dec.u32();
    auto sl = dec.u32();
    auto hi = dec.u32();
    auto ct = dec.boolean();
    if (!sid || !sq || !sl || !hi || !ct) {
      co_await rpc::Dispatcher::reply_garbage_args(conn, call.xid);
      co_return;
    }
    std::copy(sid->begin(), sid->end(), ctx.sessionid.begin());
    ctx.slotid = *sl;
    ctx.seqid = *sq;
    ctx.cachethis = *ct;

    if (*numops > state_.config().max_ops) {  // global cap >= every session's cap
      enc.u32(static_cast<uint32_t>(Op::kSequence));
      enc.u32(st(Status::kTooManyOps));
      co_await finish(st(Status::kTooManyOps), 1);
      co_return;
    }
    auto seq = co_await state_.sequence_begin(ctx.sessionid, *sl, *sq, *hi, *ct,
                                              conn_id_of(conn));
    if (seq.replay && !seq.replay_bytes.empty()) {
      // Exactly-once: the cached COMPOUND body is replayed under a fresh RPC header —
      // retransmissions may arrive with a new xid (RFC 8881 §2.10.6.2 replays are an
      // NFS-level, not RPC-level, contract).
      xdr::XdrEnc replay(conn.pool);
      rpc::encode_reply_success(replay, call.xid);
      replay.opaque_fixed(seq.replay_bytes);
      co_await conn.send(replay.take());
      co_return;
    }
    if (seq.status != 0 || seq.replay) {
      uint32_t code = seq.status ? seq.status : st(Status::kRetryUncachedRep);
      enc.u32(static_cast<uint32_t>(Op::kSequence));
      enc.u32(code);
      co_await finish(code, 1);
      co_return;
    }

    if (request_size > seq.max_request) {
      co_await state_.sequence_abort(ctx.sessionid, ctx.slotid);
      enc.u32(static_cast<uint32_t>(Op::kSequence));
      enc.u32(st(Status::kReqTooBig));
      co_await finish(st(Status::kReqTooBig), 1);
      co_return;
    }
    if (*numops > seq.max_ops) {
      co_await state_.sequence_abort(ctx.sessionid, ctx.slotid);
      enc.u32(static_cast<uint32_t>(Op::kSequence));
      enc.u32(st(Status::kTooManyOps));
      co_await finish(st(Status::kTooManyOps), 1);
      co_return;
    }
    if (ctx.cachethis && seq.max_response_cached < 128) {
      // Cannot cache even the bare SEQUENCE result: refuse without consuming the slot.
      co_await state_.sequence_abort(ctx.sessionid, ctx.slotid);
      enc.u32(static_cast<uint32_t>(Op::kSequence));
      enc.u32(st(Status::kRepTooBigToCache));
      co_await finish(st(Status::kRepTooBigToCache), 1);
      co_return;
    }

    // Slot claimed. Effective budget: cachethis caps the whole reply at the cached
    // limit so every cached reply fits (design 07 §7.3).
    ctx.session = true;
    std::memcpy(&ctx.clientid, ctx.sessionid.data(), 8);
    ctx.max_response = ctx.cachethis
                           ? std::min(seq.max_response, seq.max_response_cached)
                           : seq.max_response;
    enc.u32(static_cast<uint32_t>(Op::kSequence));
    enc.u32(st(Status::kOk));
    enc.opaque_fixed(ctx.sessionid);
    enc.u32(*sq);
    enc.u32(*sl);
    enc.u32(seq.highest_slot);
    enc.u32(seq.highest_slot);
    enc.u32(0);  // sr_status_flags
    status = st(Status::kOk);
    done = 1;
    bool saw_destroy_session = false;

    for (uint32_t i = 1; i < *numops && status == st(Status::kOk); ++i) {
      auto opcode = dec.u32();
      if (!opcode) {
        status = st(Status::kBadxdr);
        break;
      }
      Op op = static_cast<Op>(*opcode);
      if (saw_destroy_session || op == Op::kBindConnToSession) {
        // BIND_CONN_TO_SESSION never rides a session compound; DESTROY_SESSION may,
        // but only as the final operation (RFC 8881 §18.34/18.37).
        enc.u32(*opcode);
        enc.u32(st(Status::kNotOnlyOp));
        status = st(Status::kNotOnlyOp);
        ++done;
        break;
      }
      if (op == Op::kDestroySession) saw_destroy_session = true;
      if (op == Op::kRead || op == Op::kReaddir) {
        // These budget themselves against ctx.max_response (clamp / truncate).
        status = co_await exec_op(ctx, *opcode, dec, enc);
        ++done;
        continue;
      }
      // Everything else is encoded via a staging buffer so an over-budget reply can be
      // replaced by REP_TOO_BIG(_TO_CACHE) for exactly this op (RFC 8881 §2.10.6.4).
      xdr::XdrEnc staged(conn.pool);
      status = co_await exec_op(ctx, *opcode, dec, staged);
      auto bytes = staged.take().to_bytes();
      if (enc.size() + bytes.size() + 16 > ctx.max_response) {
        status = ctx.cachethis && enc.size() + bytes.size() + 16 <= seq.max_response
                     ? st(Status::kRepTooBigToCache)
                     : st(Status::kRepTooBig);
        enc.u32(*opcode);
        enc.u32(status);
        ++done;
        break;
      }
      enc.opaque_fixed(bytes);
      ++done;
    }

    // complete-then-send: the slot must be updated before the client can react to
    // the reply (a fast retransmit against a not-yet-updated slot would misorder).
    patch_u32(status_gap, status);
    patch_u32(count_gap, done);
    auto buf = enc.take();
    std::vector<std::byte> cache_bytes;
    if (ctx.cachethis) {
      auto all = buf.to_bytes();
      // Strip the 24-byte RPC reply header: replays re-encode it for their own xid.
      cache_bytes.assign(all.begin() + 24, all.end());
    }
    co_await state_.sequence_complete(ctx.sessionid, ctx.slotid, ctx.seqid,
                                      ctx.cachethis, std::move(cache_bytes));
    co_await conn.send(std::move(buf));
    co_return;
  }

  if (sessionless_op(*first_op)) {
    if (*numops > 1) {  // sessionless ops form solo compounds (RFC 8881 §18)
      enc.u32(*first_op);
      enc.u32(st(Status::kNotOnlyOp));
      co_await finish(st(Status::kNotOnlyOp), 1);
      co_return;
    }
    status = co_await exec_op(ctx, *first_op, dec, enc);
    co_await finish(status, 1);
    co_return;
  }

  // Unknown opcodes answer OP_ILLEGAL even ahead of session discipline.
  if (*first_op < kFirstOp || *first_op > kLastKnownOp) {
    enc.u32(static_cast<uint32_t>(Op::kIllegal));
    enc.u32(st(Status::kOpIllegal));
    co_await finish(st(Status::kOpIllegal), 1);
    co_return;
  }
  // Any other first op requires a session.
  enc.u32(*first_op);
  enc.u32(st(Status::kOpNotInSession));
  co_await finish(st(Status::kOpNotInSession), 1);
}

// ---- op dispatch -----------------------------------------------------------

rt::Task<uint32_t> Engine::exec_op(Ctx& ctx, uint32_t opcode, xdr::XdrDec& dec,
                                   xdr::XdrEnc& enc) {
  switch (static_cast<Op>(opcode)) {
    case Op::kPutrootfh:
    case Op::kPutpubfh: {
      enc.u32(opcode);
      co_return co_await op_putrootfh(ctx, enc);
    }
    case Op::kPutfh: {
      enc.u32(opcode);
      co_return co_await op_putfh(ctx, dec, enc);
    }
    case Op::kGetfh: {
      enc.u32(opcode);
      co_return op_getfh(ctx, enc);
    }
    case Op::kSavefh: {
      enc.u32(opcode);
      if (ctx.cfh.empty()) {
        enc.u32(st(Status::kNofilehandle));
        co_return st(Status::kNofilehandle);
      }
      ctx.sfh = ctx.cfh;
      enc.u32(st(Status::kOk));
      co_return st(Status::kOk);
    }
    case Op::kRestorefh: {
      enc.u32(opcode);
      if (ctx.sfh.empty()) {
        enc.u32(st(Status::kRestorefh));
        co_return st(Status::kRestorefh);
      }
      ctx.cfh = ctx.sfh;
      enc.u32(st(Status::kOk));
      co_return st(Status::kOk);
    }
    case Op::kLookup: {
      enc.u32(opcode);
      co_return co_await op_lookup(ctx, dec, enc);
    }
    case Op::kLookupp: {
      enc.u32(opcode);
      co_return co_await op_lookupp(ctx, enc);
    }
    case Op::kGetattr: {
      enc.u32(opcode);
      co_return co_await op_getattr(ctx, dec, enc);
    }
    case Op::kAccess: {
      enc.u32(opcode);
      co_return co_await op_access(ctx, dec, enc);
    }
    case Op::kReadlink: {
      enc.u32(opcode);
      co_return co_await op_readlink(ctx, enc);
    }
    case Op::kRead: {
      enc.u32(opcode);
      co_return co_await op_read(ctx, dec, enc);
    }
    case Op::kReaddir: {
      enc.u32(opcode);
      co_return co_await op_readdir(ctx, dec, enc);
    }
    case Op::kOpen: {
      enc.u32(opcode);
      co_return co_await op_open(ctx, dec, enc);
    }
    case Op::kClose: {
      enc.u32(opcode);
      co_return co_await op_close(ctx, dec, enc);
    }
    case Op::kSecinfoNoName: {
      enc.u32(opcode);
      co_return co_await op_secinfo_no_name(ctx, dec, enc);
    }
    case Op::kFreeStateid: {
      enc.u32(opcode);
      co_return co_await op_free_stateid(ctx, dec, enc);
    }
    case Op::kTestStateid: {
      enc.u32(opcode);
      co_return co_await op_test_stateid(ctx, dec, enc);
    }
    case Op::kReclaimComplete: {
      enc.u32(opcode);
      co_return co_await op_reclaim_complete(ctx, dec, enc);
    }
    case Op::kExchangeId: {
      enc.u32(opcode);
      co_return co_await op_exchange_id(ctx, dec, enc);
    }
    case Op::kCreateSession: {
      enc.u32(opcode);
      co_return co_await op_create_session(ctx, dec, enc);
    }
    case Op::kDestroySession: {
      enc.u32(opcode);
      co_return co_await op_destroy_session(ctx, dec, enc);
    }
    case Op::kDestroyClientid: {
      enc.u32(opcode);
      co_return co_await op_destroy_clientid(ctx, dec, enc);
    }
    case Op::kBindConnToSession: {
      enc.u32(opcode);
      co_return co_await op_bind_conn(ctx, dec, enc);
    }
    case Op::kSequence: {  // SEQUENCE anywhere but first
      enc.u32(opcode);
      enc.u32(st(Status::kSequencePos));
      co_return st(Status::kSequencePos);
    }
    default: {
      if (opcode >= kFirstOp && opcode <= kLastKnownOp) {
        enc.u32(opcode);
        enc.u32(st(Status::kNotsupp));
        co_return st(Status::kNotsupp);
      }
      enc.u32(static_cast<uint32_t>(Op::kIllegal));
      enc.u32(st(Status::kOpIllegal));
      co_return st(Status::kOpIllegal);
    }
  }
}

// ---- filehandle ops --------------------------------------------------------

rt::Task<uint32_t> Engine::op_putrootfh(Ctx& ctx, xdr::XdrEnc& enc) {
  core::PseudoFs::Node* root = pseudo_.root();
  if (root->exp) {  // "/" itself is an export: PUTROOTFH lands on the export root
    if (!exports_.check_client(ctx.conn.peer.addr, *root->exp)) {
      enc.u32(st(Status::kAccess));
      co_return st(Status::kAccess);
    }
    auto obj = co_await root->exp->backend->root();
    if (!obj) {
      uint32_t code = st(core::to_v4(obj.error(), Op::kPutrootfh));
      enc.u32(code);
      co_return code;
    }
    ctx.cfh = export_fh(*root->exp, (*obj)->id());
  } else {
    ctx.cfh = pseudo_fh(*root);
  }
  enc.u32(st(Status::kOk));
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_putfh(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto fh = dec.opaque(kMaxFileHandle);
  if (!fh) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  FhBytes bytes(fh->begin(), fh->end());
  auto decoded = handles_.decode_v4(bytes, ctx.conn.peer.addr);
  if (!decoded) {
    uint32_t code = st(core::to_v4(decoded.error(), Op::kPutfh));
    enc.u32(code);
    co_return code;
  }
  ctx.cfh = std::move(bytes);
  enc.u32(st(Status::kOk));
  co_return st(Status::kOk);
}

uint32_t Engine::op_getfh(Ctx& ctx, xdr::XdrEnc& enc) {
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    return st(Status::kNofilehandle);
  }
  enc.u32(st(Status::kOk));
  enc.opaque(ctx.cfh);
  return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_lookup(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto name = dec.string(kMaxName + 1);
  if (!name) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  if (name->empty()) {  // zero-length component: INVAL, not BADNAME (RFC 8881 §18.10)
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  if (!valid_component4(*name)) {
    enc.u32(st(Status::kBadname));
    co_return st(Status::kBadname);
  }
  auto resolved = co_await resolve(ctx.cfh, ctx.conn.peer.addr);
  if (!resolved) {
    uint32_t code = st(core::to_v4(resolved.error(), Op::kLookup));
    enc.u32(code);
    co_return code;
  }
  if (resolved->pseudo()) {
    auto it = resolved->node->children.find(*name);
    if (it == resolved->node->children.end()) {
      enc.u32(st(Status::kNoent));
      co_return st(Status::kNoent);
    }
    core::PseudoFs::Node* child = it->second.get();
    if (child->exp) {  // crossing into an export: the CIDR gate applies here
      if (!exports_.check_client(ctx.conn.peer.addr, *child->exp)) {
        enc.u32(st(Status::kAccess));
        co_return st(Status::kAccess);
      }
      auto obj = co_await child->exp->backend->root();
      if (!obj) {
        uint32_t code = st(core::to_v4(obj.error(), Op::kLookup));
        enc.u32(code);
        co_return code;
      }
      ctx.cfh = export_fh(*child->exp, (*obj)->id());
    } else {
      ctx.cfh = pseudo_fh(*child);
    }
    enc.u32(st(Status::kOk));
    co_return st(Status::kOk);
  }
  auto mapped = exports_.squash_cred(ctx.cred, *resolved->exp);
  auto cred = mapped.view();
  auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
  auto held = co_await lock->lock_shared();
  auto found = co_await resolved->obj->lookup(cred, *name);
  if (!found) {
    uint32_t code = st(core::to_v4(found.error(), Op::kLookup));
    enc.u32(code);
    co_return code;
  }
  ctx.cfh = export_fh(*resolved->exp, (*found)->id());
  enc.u32(st(Status::kOk));
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_lookupp(Ctx& ctx, xdr::XdrEnc& enc) {
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  auto resolved = co_await resolve(ctx.cfh, ctx.conn.peer.addr);
  if (!resolved) {
    uint32_t code = st(core::to_v4(resolved.error(), Op::kLookupp));
    enc.u32(code);
    co_return code;
  }
  if (resolved->pseudo()) {
    if (!resolved->node->parent) {  // pseudo root has no parent
      enc.u32(st(Status::kNoent));
      co_return st(Status::kNoent);
    }
    ctx.cfh = pseudo_fh(*resolved->node->parent);
    enc.u32(st(Status::kOk));
    co_return st(Status::kOk);
  }
  if (resolved->obj->type() == backend::FType::kLnk) {
    enc.u32(st(Status::kSymlink));  // LOOKUPP through a symlink (RFC 8881 §18.14)
    co_return st(Status::kSymlink);
  }
  if (resolved->obj->type() != backend::FType::kDir) {
    enc.u32(st(Status::kNotdir));
    co_return st(Status::kNotdir);
  }
  // At the export root, the parent lives in the pseudo tree.
  auto root_obj = co_await resolved->exp->backend->root();
  if (root_obj && (*root_obj)->id() == resolved->oid) {
    core::PseudoFs::Node* crossing = pseudo_.for_export(resolved->exp->fsid);
    if (!crossing || !crossing->parent) {
      enc.u32(st(Status::kNoent));
      co_return st(Status::kNoent);
    }
    ctx.cfh = pseudo_fh(*crossing->parent);
    enc.u32(st(Status::kOk));
    co_return st(Status::kOk);
  }
  auto mapped = exports_.squash_cred(ctx.cred, *resolved->exp);
  auto cred = mapped.view();
  auto parent = co_await resolved->obj->lookup(cred, "..");
  if (!parent) {
    uint32_t code = st(core::to_v4(parent.error(), Op::kLookupp));
    enc.u32(code);
    co_return code;
  }
  ctx.cfh = export_fh(*resolved->exp, (*parent)->id());
  enc.u32(st(Status::kOk));
  co_return st(Status::kOk);
}

// ---- attributes ------------------------------------------------------------

rt::Task<uint32_t> Engine::attr_reply(Ctx& ctx, const Resolved& resolved,
                                      const Bitmap& wanted, xdr::XdrEnc& enc) {
  AttrSource src;
  backend::Attr attr;
  backend::FsLimits limits;
  backend::FsStats stats;
  if (resolved.pseudo()) {
    attr = pseudo_.attr_of(*resolved.node);
    src.fsid = 0;
    src.link_support = false;
    src.symlink_support = false;
  } else {
    auto lock = locks_.get(resolved.exp->fsid, resolved.oid);
    auto held = co_await lock->lock_shared();
    auto got = co_await resolved.obj->getattr();
    if (!got) co_return st(core::to_v4(got.error(), Op::kGetattr));
    attr = *got;
    limits = resolved.exp->backend->limits();
    src.limits = &limits;
    src.fsid = resolved.exp->fsid;
    auto caps = resolved.exp->backend->caps();
    src.link_support = caps.has(backend::Cap::kHardlink);
    src.symlink_support = caps.has(backend::Cap::kSymlink);
    src.case_insensitive = caps.has(backend::Cap::kCaseInsensitive);
    if (wants_stats(wanted)) {
      auto s = co_await resolved.exp->backend->statfs();
      if (s) {
        stats = *s;
        src.stats = &stats;
      }
    }
    if (wanted.test(attr::kMountedOnFileid)) {
      auto root_obj = co_await resolved.exp->backend->root();
      if (root_obj && (*root_obj)->id() == resolved.oid) {
        if (auto* crossing = pseudo_.for_export(resolved.exp->fsid))
          src.mounted_on_fileid = crossing->id;
      }
    }
  }
  src.attr = &attr;
  src.fh = ctx.cfh;
  enc.u32(st(Status::kOk));
  encode_fattr(enc, wanted, src, ctx.conn.pool);
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_getattr(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto wanted = Bitmap::decode(dec);
  if (!wanted) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  auto resolved = co_await resolve(ctx.cfh, ctx.conn.peer.addr);
  if (!resolved) {
    uint32_t code = st(core::to_v4(resolved.error(), Op::kGetattr));
    enc.u32(code);
    co_return code;
  }
  uint32_t code = co_await attr_reply(ctx, *resolved, *wanted, enc);
  if (code != st(Status::kOk)) enc.u32(code);  // attr_reply encodes only on success
  co_return code;
}

rt::Task<uint32_t> Engine::op_access(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto mask = dec.u32();
  if (!mask) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  auto resolved = co_await resolve(ctx.cfh, ctx.conn.peer.addr);
  if (!resolved) {
    uint32_t code = st(core::to_v4(resolved.error(), Op::kAccess));
    enc.u32(code);
    co_return code;
  }
  uint32_t supported = kAccessRead | kAccessLookup | kAccessModify | kAccessExtend |
                       kAccessDelete | kAccessExecute;
  uint32_t granted = 0;
  if (resolved->pseudo()) {
    supported = kAccessRead | kAccessLookup | kAccessExecute;
    granted = supported;
  } else {
    auto mapped = exports_.squash_cred(ctx.cred, *resolved->exp);
    auto cred = mapped.view();
    auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
    auto held = co_await lock->lock_shared();
    backend::AccessMask want;
    want.set(backend::Access::kRead)
        .set(backend::Access::kLookup)
        .set(backend::Access::kModify)
        .set(backend::Access::kExtend)
        .set(backend::Access::kDelete)
        .set(backend::Access::kExecute);
    auto allowed = co_await resolved->obj->access(cred, want);
    if (!allowed) {
      uint32_t code = st(core::to_v4(allowed.error(), Op::kAccess));
      enc.u32(code);
      co_return code;
    }
    if (resolved->exp->readonly)
      allowed->clear(backend::Access::kModify)
          .clear(backend::Access::kExtend)
          .clear(backend::Access::kDelete);
    if (allowed->has(backend::Access::kRead)) granted |= kAccessRead;
    if (allowed->has(backend::Access::kLookup)) granted |= kAccessLookup;
    if (allowed->has(backend::Access::kModify)) granted |= kAccessModify;
    if (allowed->has(backend::Access::kExtend)) granted |= kAccessExtend;
    if (allowed->has(backend::Access::kDelete)) granted |= kAccessDelete;
    if (allowed->has(backend::Access::kExecute)) granted |= kAccessExecute;
  }
  enc.u32(st(Status::kOk));
  enc.u32(supported & *mask);
  enc.u32(granted & *mask);
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_readlink(Ctx& ctx, xdr::XdrEnc& enc) {
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  auto resolved = co_await resolve(ctx.cfh, ctx.conn.peer.addr);
  if (!resolved) {
    uint32_t code = st(core::to_v4(resolved.error(), Op::kReadlink));
    enc.u32(code);
    co_return code;
  }
  if (resolved->pseudo()) {
    enc.u32(st(Status::kWrongType));
    co_return st(Status::kWrongType);
  }
  auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
  auto held = co_await lock->lock_shared();
  auto target = co_await resolved->obj->readlink();
  if (!target) {
    uint32_t code = st(core::to_v4(target.error(), Op::kReadlink));
    enc.u32(code);
    co_return code;
  }
  enc.u32(st(Status::kOk));
  enc.string(*target);
  co_return st(Status::kOk);
}

// ---- IO --------------------------------------------------------------------

rt::Task<uint32_t> Engine::op_read(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto sid = Stateid::decode(dec);
  auto offset = dec.u64();
  auto count = dec.u32();
  if (!sid || !offset || !count) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  auto resolved = co_await resolve(ctx.cfh, ctx.conn.peer.addr);
  if (!resolved) {
    uint32_t code = st(core::to_v4(resolved.error(), Op::kRead));
    enc.u32(code);
    co_return code;
  }
  if (resolved->pseudo()) {
    enc.u32(st(Status::kIsdir));
    co_return st(Status::kIsdir);
  }
  auto check = co_await state_.lookup_stateid(*sid);
  if (check.status != 0) {
    enc.u32(check.status);
    co_return check.status;
  }
  if (!check.special && !(check.rec.oid == resolved->oid)) {
    enc.u32(st(Status::kBadStateid));
    co_return st(Status::kBadStateid);
  }
  auto mapped = exports_.squash_cred(ctx.cred, *resolved->exp);
  auto cred = mapped.view();
  size_t slack = 256;  // op headers + eof/len fields
  size_t budget = ctx.max_response > enc.size() + slack
                      ? ctx.max_response - enc.size() - slack
                      : 0;
  uint32_t len = std::min<uint32_t>(
      {*count, resolved->exp->backend->limits().max_read,
       static_cast<uint32_t>(std::min<size_t>(budget, UINT32_MAX))});
  auto data = ctx.conn.pool.alloc(std::max<uint32_t>(len, 1));
  bool eof = false;
  auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
  auto held = co_await lock->lock_shared();
  backend::OpenCtx open{cred, nullptr};
  auto n = co_await resolved->obj->read(open, *offset,
                                        std::span<std::byte>(data.data(), len), eof);
  if (!n) {
    uint32_t code = st(core::to_v4(n.error(), Op::kRead));
    enc.u32(code);
    co_return code;
  }
  obs::Metrics::instance().read_bytes.fetch_add(*n, std::memory_order_relaxed);
  enc.u32(st(Status::kOk));
  enc.boolean(eof);
  enc.u32(*n);
  enc.attach(std::move(data), 0, *n);
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_readdir(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto cookie = dec.u64();
  auto verf = dec.opaque_fixed(8);
  auto dircount = dec.u32();
  auto maxcount = dec.u32();
  auto wanted = Bitmap::decode(dec);
  if (!cookie || !verf || !dircount || !maxcount || !wanted) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  if (*cookie == 1 || *cookie == 2) {  // reserved cookie space
    enc.u32(st(Status::kBadCookie));
    co_return st(Status::kBadCookie);
  }
  auto resolved = co_await resolve(ctx.cfh, ctx.conn.peer.addr);
  if (!resolved) {
    uint32_t code = st(core::to_v4(resolved.error(), Op::kReaddir));
    enc.u32(code);
    co_return code;
  }

  size_t slack = 256;
  size_t budget = ctx.max_response > enc.size() + slack
                      ? std::min<size_t>(*maxcount, ctx.max_response - enc.size() - slack)
                      : 0;
  if (budget < 128) {
    enc.u32(st(Status::kToosmall));
    co_return st(Status::kToosmall);
  }

  // Export side: fetch the first page before committing to an OK status, so cursor
  // errors (BAD_COOKIE) can still be reported cleanly.
  backend::DirPage first_page;
  core::MappedCred mapped;
  backend::Cred cred{};
  backend::FsLimits limits;
  bool link_sup = false, symlink_sup = false;
  std::optional<decltype(locks_.get(0, backend::ObjId{})->lock_shared())> unused;
  std::shared_ptr<rt::AsyncSharedMutex> lock;
  if (!resolved->pseudo()) {
    mapped = exports_.squash_cred(ctx.cred, *resolved->exp);
    cred = mapped.view();
    limits = resolved->exp->backend->limits();
    auto caps = resolved->exp->backend->caps();
    link_sup = caps.has(backend::Cap::kHardlink);
    symlink_sup = caps.has(backend::Cap::kSymlink);
    lock = locks_.get(resolved->exp->fsid, resolved->oid);
    auto held = co_await lock->lock_shared();
    auto page = co_await core::readdir_page(resolved->obj, cred, *cookie, 128);
    if (!page) {
      uint32_t code = *cookie != 0 && page.error() == errno_from(EINVAL)
                          ? st(Status::kBadCookie)
                          : st(core::to_v4(page.error(), Op::kReaddir));
      enc.u32(code);
      co_return code;
    }
    first_page = std::move(*page);
  }

  enc.u32(st(Status::kOk));
  std::array<std::byte, 8> zero_verf{};
  enc.opaque_fixed(zero_verf);
  size_t used = 32;       // verifier + list/eof framing
  size_t used_dir = 0;    // dircount budget: cookie+name portions
  bool truncated = false;
  bool eof = false;

  auto emit = [&](uint64_t entry_cookie, std::string_view name,
                  const backend::Attr& attr, const FhBytes& fh, uint64_t fsid_val,
                  uint64_t mounted_on, const backend::FsLimits* lim_ptr,
                  bool ent_link_sup, bool ent_symlink_sup) -> bool {
    xdr::XdrEnc entry(ctx.conn.pool);
    entry.boolean(true);
    entry.u64(entry_cookie);
    entry.string(name);
    AttrSource src;
    src.attr = &attr;
    src.fsid = fsid_val;
    src.mounted_on_fileid = mounted_on;
    src.fh = fh;
    src.limits = lim_ptr;
    src.link_support = ent_link_sup;
    src.symlink_support = ent_symlink_sup;
    encode_fattr(entry, *wanted, src, ctx.conn.pool);
    auto bytes = entry.take().to_bytes();
    size_t name_part = 8 + 4 + ((name.size() + 3) & ~size_t(3));
    if (used + bytes.size() > budget || used_dir + name_part > *dircount) {
      truncated = true;
      return false;
    }
    enc.opaque_fixed(bytes);  // raw append: already XDR-formed
    used += bytes.size();
    used_dir += name_part;
    return true;
  };

  if (resolved->pseudo()) {
    uint64_t index = 3;
    eof = true;
    for (const auto& [name, child] : resolved->node->children) {
      uint64_t this_cookie = index++;
      if (*cookie >= this_cookie) continue;
      bool ok;
      if (child->exp) {
        if (!exports_.check_client(ctx.conn.peer.addr, *child->exp)) continue;
        auto obj = co_await child->exp->backend->root();
        if (!obj) continue;
        auto attr = co_await (*obj)->getattr();
        if (!attr) continue;
        backend::FsLimits child_limits = child->exp->backend->limits();
        auto caps = child->exp->backend->caps();
        ok = emit(this_cookie, name, *attr, export_fh(*child->exp, (*obj)->id()),
                  child->exp->fsid, child->id, &child_limits,
                  caps.has(backend::Cap::kHardlink), caps.has(backend::Cap::kSymlink));
      } else {
        auto attr = pseudo_.attr_of(*child);
        ok = emit(this_cookie, name, attr, pseudo_fh(*child), 0, child->id, nullptr,
                  false, false);
      }
      if (!ok) {
        eof = false;
        break;
      }
    }
  } else {
    auto held = co_await lock->lock_shared();
    backend::DirPage page = std::move(first_page);
    for (;;) {
      if (page.ents.empty()) {
        eof = page.eof;
        break;
      }
      uint64_t next_cookie = *cookie;
      for (auto& ent : page.ents) {
        next_cookie = ent.cookie;
        if (ent.name == "." || ent.name == "..") continue;  // v4 lists no dot entries
        backend::Attr attr;
        backend::ObjId oid;
        if (ent.attr && ent.oid) {
          attr = *ent.attr;
          oid = *ent.oid;
        } else {
          auto child = co_await resolved->obj->lookup(cred, ent.name);
          if (!child) continue;
          auto got = co_await (*child)->getattr();
          if (!got) continue;
          attr = *got;
          oid = (*child)->id();
        }
        if (!emit(ent.cookie, ent.name, attr, export_fh(*resolved->exp, oid),
                  resolved->exp->fsid, attr.fileid, &limits, link_sup, symlink_sup))
          break;
      }
      if (truncated) break;
      if (page.eof) {
        eof = true;
        break;
      }
      auto next = co_await core::readdir_page(resolved->obj, cred, next_cookie, 128);
      if (!next) break;  // mid-stream error: return what we have, eof=false
      page = std::move(*next);
    }
  }
  enc.boolean(false);  // end of entries
  enc.boolean(eof && !truncated);
  co_return st(Status::kOk);
}

// ---- minimal open-state ops ------------------------------------------------

rt::Task<uint32_t> Engine::op_open(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto seqid = dec.u32();
  auto share_access = dec.u32();
  auto share_deny = dec.u32();
  auto owner_client = dec.u64();
  auto owner = dec.opaque(kMaxOwnerId);
  auto opentype = dec.u32();
  if (!seqid || !share_access || !share_deny || !owner_client || !owner || !opentype) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (!ctx.session) {
    enc.u32(st(Status::kOpNotInSession));
    co_return st(Status::kOpNotInSession);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  uint32_t access = *share_access & 0x3;  // high bits carry deleg-want flags: ignored
  if (*opentype == 1) {  // OPEN4_CREATE on the read-only milestone
    enc.u32(st(Status::kRofs));
    co_return st(Status::kRofs);
  }
  if (*opentype != 0) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  auto claim = dec.u32();
  if (!claim) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }

  auto dir = co_await resolve(ctx.cfh, ctx.conn.peer.addr);
  if (!dir) {
    uint32_t code = st(core::to_v4(dir.error(), Op::kOpen));
    enc.u32(code);
    co_return code;
  }
  if (dir->pseudo() && *claim == 0) {
    enc.u32(st(Status::kRofs));  // cannot open files in the synthesized tree
    co_return st(Status::kRofs);
  }

  backend::ObjPtr file;
  uint64_t change_before = 0, change_after = 0;
  core::ExportEntry* exp = dir->exp;
  if (*claim == 0) {  // CLAIM_NULL: CFH = directory, name follows
    auto name = dec.string(kMaxName + 1);
    if (!name) {
      enc.u32(st(Status::kBadxdr));
      co_return st(Status::kBadxdr);
    }
    if (!valid_component4(*name)) {
      enc.u32(st(Status::kBadname));
      co_return st(Status::kBadname);
    }
    auto mapped = exports_.squash_cred(ctx.cred, *exp);
    auto cred = mapped.view();
    auto lock = locks_.get(exp->fsid, dir->oid);
    auto held = co_await lock->lock_shared();
    auto before = co_await dir->obj->getattr();
    if (before) change_before = before->change;
    auto found = co_await dir->obj->lookup(cred, *name);
    auto after = co_await dir->obj->getattr();
    if (after) change_after = after->change;
    if (!found) {
      uint32_t code = st(core::to_v4(found.error(), Op::kOpen));
      enc.u32(code);
      co_return code;
    }
    file = std::move(*found);
  } else if (*claim == 1) {  // CLAIM_PREVIOUS: reclaim after restart
    auto deleg = dec.u32();
    (void)deleg;
    if (!state_.in_grace()) {
      enc.u32(st(Status::kNoGrace));
      co_return st(Status::kNoGrace);
    }
    // Phase 3 carries no reopenable state across restarts (read-only opens are not
    // persisted); an honest RECLAIM_BAD sends the client to a fresh CLAIM_NULL.
    enc.u32(st(Status::kReclaimBad));
    co_return st(Status::kReclaimBad);
  } else if (*claim == 4) {  // CLAIM_FH: CFH is the file itself
    if (dir->pseudo()) {
      enc.u32(st(Status::kRofs));
      co_return st(Status::kRofs);
    }
    file = dir->obj;
  } else {
    enc.u32(st(Status::kNotsupp));
    co_return st(Status::kNotsupp);
  }

  if (file->type() == backend::FType::kDir) {
    enc.u32(st(Status::kIsdir));
    co_return st(Status::kIsdir);
  }
  if (file->type() == backend::FType::kLnk) {
    enc.u32(st(Status::kSymlink));
    co_return st(Status::kSymlink);
  }
  if ((access & 0x2) && exp->readonly) {  // WRITE access on a read-only export
    enc.u32(st(Status::kRofs));
    co_return st(Status::kRofs);
  }

  auto stateid = co_await state_.open_read(ctx.clientid, exp->fsid, file->id());
  ctx.cfh = export_fh(*exp, file->id());
  enc.u32(st(Status::kOk));
  stateid.encode(enc);
  enc.boolean(false);  // change_info4.atomic
  enc.u64(change_before);
  enc.u64(change_after);
  enc.u32(0x4);  // OPEN4_RESULT_LOCKTYPE_POSIX
  Bitmap attrset;
  attrset.encode(enc);
  enc.u32(0);  // open_delegation4: OPEN_DELEGATE_NONE
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_close(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto seqid = dec.u32();
  auto sid = Stateid::decode(dec);
  if (!seqid || !sid) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  uint32_t code = co_await state_.close_state(*sid);
  if (code != 0) {
    enc.u32(code);
    co_return code;
  }
  enc.u32(st(Status::kOk));
  Stateid out = *sid;
  out.seqid += 1;
  out.encode(enc);
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_secinfo_no_name(Ctx& ctx, xdr::XdrDec& dec,
                                              xdr::XdrEnc& enc) {
  auto style = dec.u32();
  if (!style || *style > 1) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  if (*style == 1) {  // SECINFO_STYLE4_PARENT: answer for the parent directory
    auto resolved = co_await resolve(ctx.cfh, ctx.conn.peer.addr);
    if (!resolved) {
      uint32_t code = st(core::to_v4(resolved.error(), Op::kSecinfoNoName));
      enc.u32(code);
      co_return code;
    }
    if (resolved->pseudo() && !resolved->node->parent) {
      enc.u32(st(Status::kNoent));  // the pseudo root has no parent
      co_return st(Status::kNoent);
    }
  }
  enc.u32(st(Status::kOk));
  enc.u32(1);  // one flavor
  enc.u32(1);  // AUTH_SYS
  ctx.cfh.clear();  // SECINFO consumes the current filehandle
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_free_stateid(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto sid = Stateid::decode(dec);
  if (!sid) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  uint32_t code = co_await state_.free_stateid(*sid);
  enc.u32(code);
  co_return code;
}

rt::Task<uint32_t> Engine::op_test_stateid(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto count = dec.u32();
  if (!count || *count > 64) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  SmallVec<uint32_t, 8> results;
  for (uint32_t i = 0; i < *count; ++i) {
    auto sid = Stateid::decode(dec);
    if (!sid) {
      enc.u32(st(Status::kBadxdr));
      co_return st(Status::kBadxdr);
    }
    auto check = co_await state_.lookup_stateid(*sid);
    results.push_back(check.special ? st(Status::kBadStateid) : check.status);
  }
  enc.u32(st(Status::kOk));
  enc.u32(static_cast<uint32_t>(results.size()));
  for (uint32_t r : results) enc.u32(r);
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_reclaim_complete(Ctx& ctx, xdr::XdrDec& dec,
                                               xdr::XdrEnc& enc) {
  auto one_fs = dec.boolean();
  if (!one_fs) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (!ctx.session) {
    enc.u32(st(Status::kOpNotInSession));
    co_return st(Status::kOpNotInSession);
  }
  if (*one_fs) {  // fs-scoped completion: accepted, only the global flag is tracked
    enc.u32(st(Status::kOk));
    co_return st(Status::kOk);
  }
  uint32_t code = co_await state_.reclaim_complete(ctx.clientid);
  enc.u32(code);
  co_return code;
}

// ---- sessionless operations ------------------------------------------------

rt::Task<uint32_t> Engine::op_exchange_id(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto verf = dec.opaque_fixed(8);
  auto ownerid = dec.opaque(kMaxOwnerId);
  auto flags = dec.u32();
  auto sp_how = dec.u32();
  if (!verf || !ownerid || !flags || !sp_how) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if ((*flags & kEidConfirmedR) || (*flags & ~(kEidValidRequest | kEidConfirmedR))) {
    enc.u32(st(Status::kInval));  // reply-only or undefined flag bits
    co_return st(Status::kInval);
  }
  if (*sp_how != 0) {  // SP4_NONE only (nfsv4 research 06 §6.5)
    enc.u32(st(Status::kNotsupp));
    co_return st(Status::kNotsupp);
  }
  auto impl_count = dec.u32();
  if (!impl_count || *impl_count > 1) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (*impl_count == 1) {
    auto domain = dec.string(1024);
    auto name = dec.string(1024);
    auto sec = dec.u64();
    auto nsec = dec.u32();
    if (!domain || !name || !sec || !nsec) {
      enc.u32(st(Status::kBadxdr));
      co_return st(Status::kBadxdr);
    }
  }

  Verifier verifier{};
  std::copy(verf->begin(), verf->end(), verifier.begin());
  auto result = co_await state_.exchange_id(
      std::string(reinterpret_cast<const char*>(ownerid->data()), ownerid->size()),
      verifier, ctx.cred.principal(), (*flags & kEidUpdate) != 0);
  if (result.status != 0) {
    enc.u32(result.status);
    co_return result.status;
  }
  enc.u32(st(Status::kOk));
  enc.u64(result.clientid);
  enc.u32(result.sequenceid);
  enc.u32(0x00010000 | (result.confirmed_r ? kEidConfirmedR : 0));
  enc.u32(0);           // SP4_NONE
  enc.u64(0);           // server_owner.minor_id
  enc.string("lightnfs");  // server_owner.major_id (stable across restarts)
  enc.string("lightnfs");  // server_scope
  enc.u32(0);              // server_impl_id: empty
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_create_session(Ctx& ctx, xdr::XdrDec& dec,
                                             xdr::XdrEnc& enc) {
  auto clientid = dec.u64();
  auto sequence = dec.u32();
  auto flags = dec.u32();
  auto fore = ChannelAttrs::decode(dec);
  auto back = ChannelAttrs::decode(dec);
  auto cb_program = dec.u32();
  auto sec_count = dec.u32();
  if (!clientid || !sequence || !flags || !fore || !back || !cb_program || !sec_count ||
      *sec_count > 8) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  for (uint32_t i = 0; i < *sec_count; ++i) {
    auto flavor = dec.u32();
    if (!flavor) {
      enc.u32(st(Status::kBadxdr));
      co_return st(Status::kBadxdr);
    }
    if (*flavor == 1) {  // AUTH_SYS callback cred
      auto stamp = dec.u32();
      auto machine = dec.string(255);
      auto uid = dec.u32();
      auto gid = dec.u32();
      auto gids = dec.u32();
      if (!stamp || !machine || !uid || !gid || !gids || *gids > 16) {
        enc.u32(st(Status::kBadxdr));
        co_return st(Status::kBadxdr);
      }
      for (uint32_t g = 0; g < *gids; ++g)
        if (!dec.u32()) {
          enc.u32(st(Status::kBadxdr));
          co_return st(Status::kBadxdr);
        }
    } else if (*flavor == 6) {  // RPCSEC_GSS cb parms: parsed, never used (7.7)
      auto service = dec.u32();
      auto h1 = dec.opaque(1024);
      auto h2 = dec.opaque(1024);
      if (!service || !h1 || !h2) {
        enc.u32(st(Status::kBadxdr));
        co_return st(Status::kBadxdr);
      }
    } else if (*flavor != 0) {
      enc.u32(st(Status::kBadxdr));
      co_return st(Status::kBadxdr);
    }
  }

  if (*flags & ~0x7u) {  // PERSIST | BACK_CHAN | RDMA are the only defined bits
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  if (fore->max_request < 128 || fore->max_response < 128 || back->max_request < 128 ||
      back->max_response < 128) {
    // A channel that cannot carry even a bare COMPOUND is unusable.
    enc.u32(st(Status::kToosmall));
    co_return st(Status::kToosmall);
  }
  auto result = co_await state_.create_session(*clientid, *sequence,
                                               ctx.cred.principal(), *fore, *back,
                                               conn_id_of(ctx.conn));
  if (result.status != 0) {
    enc.u32(result.status);
    co_return result.status;
  }
  if (result.replay) {
    enc.opaque_fixed(result.cached);  // previously encoded {status, body}
    co_return st(Status::kOk);
  }
  // Encode the success body, then hand a copy to the state manager for replay.
  xdr::XdrEnc body(ctx.conn.pool);
  body.u32(st(Status::kOk));
  body.opaque_fixed(result.sessionid);
  body.u32(result.sequence);
  body.u32(0);  // flags: no persist, no backchannel service
  result.fore.encode(body);
  result.back.encode(body);
  auto bytes = body.take().to_bytes();
  enc.opaque_fixed(bytes);
  co_await state_.confirm_create_session(*clientid, std::move(bytes));
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_destroy_session(Ctx& ctx, xdr::XdrDec& dec,
                                              xdr::XdrEnc& enc) {
  auto sid = dec.opaque_fixed(16);
  if (!sid) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  state::SessionId id{};
  std::copy(sid->begin(), sid->end(), id.begin());
  uint32_t code = co_await state_.destroy_session(id, conn_id_of(ctx.conn));
  enc.u32(code);
  co_return code;
}

rt::Task<uint32_t> Engine::op_destroy_clientid(Ctx& ctx, xdr::XdrDec& dec,
                                               xdr::XdrEnc& enc) {
  auto clientid = dec.u64();
  if (!clientid) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  uint32_t code = co_await state_.destroy_clientid(*clientid);
  enc.u32(code);
  co_return code;
}

rt::Task<uint32_t> Engine::op_bind_conn(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto sid = dec.opaque_fixed(16);
  auto dir = dec.u32();
  auto rdma = dec.boolean();
  if (!sid || !dir || !rdma) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  state::SessionId id{};
  std::copy(sid->begin(), sid->end(), id.begin());
  uint32_t code = co_await state_.bind_conn(id, conn_id_of(ctx.conn));
  if (code != 0) {
    enc.u32(code);
    co_return code;
  }
  enc.u32(st(Status::kOk));
  enc.opaque_fixed(id);
  enc.u32(*dir == 2 ? 2 : 1);  // grant FORE (or BACK when explicitly asked)
  enc.boolean(false);
  co_return st(Status::kOk);
}

}  // namespace lnfs::nfsv4
