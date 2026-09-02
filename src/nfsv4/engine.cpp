#include "nfsv4/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <string>

#include "core/boot_epoch.hpp"
#include "core/errmap.hpp"
#include "core/fs_props.hpp"
#include "core/mutate.hpp"
#include "core/names.hpp"
#include "transport/connection.hpp"
#include "core/readdir.hpp"
#include "nfsv4/attrs.hpp"
#include "obs/errlog.hpp"
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

using core::MutateGuard;

// utf8str_cs component discipline (RFC 8881 §14.1): malformed UTF-8 is INVAL.
bool utf8_component(std::string_view name) { return core::valid_utf8(name); }

// v4 mapping of a failed MutateGuard verdict (plan doc 10 §6.1): readonly exports are
// ROFS, empty components INVAL (RFC 8881 §18.10), every other component failure BADNAME.
uint32_t verdict_status4(const MutateGuard::Verdict& verdict) {
  switch (verdict.kind) {
    case MutateGuard::Verdict::kReadonly: return st(Status::kRofs);
    case MutateGuard::Verdict::kBadName:
      return verdict.name == core::NameCheck::kEmpty ? st(Status::kInval)
                                                     : st(Status::kBadname);
    default: return st(Status::kOk);
  }
}

bool is_current_placeholder(const Stateid& sid) {
  if (sid.seqid != 1) return false;
  return std::all_of(sid.other.begin(), sid.other.end(),
                     [](std::byte b) { return b == std::byte{0}; });
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

uint32_t Engine::resolve_current(const Ctx& ctx, Stateid& sid) {
  if (!is_current_placeholder(sid)) return st(Status::kOk);
  if (!ctx.current_valid) return st(Status::kBadStateid);
  sid = ctx.current_sid;
  return st(Status::kOk);
}

void Engine::register_with(rpc::Dispatcher& dispatcher) {
  dispatcher.add({kProgram, kVersion, kVersion, this,
                  [](void* self, ConnCtx& ctx, RpcCall& call, const rpc::Cred& cred) {
                    return static_cast<Engine*>(self)->dispatch(ctx, call, cred);
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

void Engine::configure_client_qos(uint64_t read_bps, uint64_t write_bps, uint32_t iops) {
  std::lock_guard lock(cq_mu_);
  cq_read_bps_.store(read_bps, std::memory_order_relaxed);
  cq_write_bps_.store(write_bps, std::memory_order_relaxed);
  cq_iops_.store(iops, std::memory_order_relaxed);
  for (auto& [id, qos] : cq_map_) {
    qos->read_bytes.configure(read_bps);
    qos->write_bytes.configure(write_bps);
    qos->ops.configure(iops);
  }
}

Engine::ClientQos* Engine::client_qos(uint64_t clientid) {
  if (cq_read_bps_.load(std::memory_order_relaxed) == 0 &&
      cq_write_bps_.load(std::memory_order_relaxed) == 0 &&
      cq_iops_.load(std::memory_order_relaxed) == 0)
    return nullptr;  // unconfigured: zero cost on the IO path
  std::lock_guard lock(cq_mu_);
  auto& slot = cq_map_[clientid];
  if (!slot) {
    slot = std::make_unique<ClientQos>();
    slot->read_bytes.configure(cq_read_bps_.load(std::memory_order_relaxed));
    slot->write_bytes.configure(cq_write_bps_.load(std::memory_order_relaxed));
    slot->ops.configure(cq_iops_.load(std::memory_order_relaxed));
  }
  return slot.get();
}

Engine::FhBytes Engine::pseudo_fh(const core::PseudoFs::Node& node) const {
  return handles_.encode_raw(0, core::PseudoFs::oid_of(node));
}

Engine::FhBytes Engine::export_fh(const core::ExportEntry& exp,
                                  const backend::ObjId& oid) const {
  return handles_.encode(exp, oid);
}

rt::Task<Result<Engine::Resolved>> Engine::resolve(Ctx& ctx, const FhBytes& fh) {
  // Per-COMPOUND cache (plan doc 10 §2.1): typical chains (SEQUENCE,PUTFH,GETATTR,...)
  // resolve the same filehandle once per op.  A filehandle names one object identity
  // (oids embed inode+generation), so reuse within one compound is sound; staleness
  // detection is deferred to the next compound at worst.
  if (ctx.resolved && ctx.resolved_fh == fh) co_return *ctx.resolved;
  auto decoded = handles_.decode_v4(fh, ctx.conn.peer.addr);
  if (!decoded) co_return Err(decoded.error());
  Resolved out;
  if (decoded->fsid == 0) {
    out.node = pseudo_.resolve(decoded->oid);
    if (!out.node) co_return Err(errno_from(ESTALE));
    out.oid = decoded->oid;
    ctx.resolved_fh = fh;
    ctx.resolved = out;
    co_return out;
  }
  out.exp = decoded->exp;
  auto obj = co_await out.exp->backend->resolve(decoded->oid);
  if (!obj) co_return Err(obj.error());
  out.obj = std::move(*obj);
  out.oid = decoded->oid;
  ctx.resolved_fh = fh;
  ctx.resolved = out;
  co_return out;
}

// ---- compound driver -------------------------------------------------------

rt::Task<void> Engine::compound(ConnCtx& conn, RpcCall& call, const rpc::Cred& cred) {
  auto& dec = call.args;
  auto t0 = std::chrono::steady_clock::now();
  size_t request_size = dec.remaining() + 44;  // + RPC call header approximation
  auto tag = dec.opaque(kMaxTag);
  auto minor = dec.u32();
  auto numops = dec.u32();
  if (!tag || !minor || !numops) {
    co_await rpc::Dispatcher::reply_garbage_args(conn, call.xid);
    co_return;
  }
  std::vector<std::byte> tag_bytes(tag->begin(), tag->end());
  bool tag_ok = core::valid_utf8(tag_bytes);

  Ctx ctx{.conn = conn, .cred = cred};
  ctx.minor = *minor;

  // Per-request summary line (design 08 §8.2): op names are only collected when debug
  // logging is on; the error-reply sampling ring records non-OK compounds regardless.
  const bool dbg = log_enabled(LogLevel::kDebug);
  std::string dbg_ops;
  uint32_t fail_op = 0;  // last opcode executed; 0 = failed before any op
  auto note_op = [&](uint32_t opcode) {
    fail_op = opcode;
    if (dbg) {
      if (!dbg_ops.empty()) dbg_ops += ',';
      dbg_ops += op_name(opcode);
    }
  };
  auto log_summary = [&](uint32_t status, uint32_t ops_done) {
    auto us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - t0)
                                        .count());
    obs::Metrics::instance().v4_compound_duration.observe_us(us);
    if (dbg) {
      LNFS_DEBUG("v4 xid={:#x} peer={} tag=\"{}\" minor={} ops={}/[{}] st={} dur={}us",
                 call.xid, conn.peer.to_string(),
                 std::string_view(reinterpret_cast<const char*>(tag_bytes.data()),
                                  tag_ok ? tag_bytes.size() : 0),
                 *minor, ops_done, dbg_ops, status, us);
    }
    if (status != 0)
      obs::record_error_reply(conn.peer.to_string(),
                              fail_op ? op_name(fail_op) : "COMPOUND", call.xid, status);
    // Slow-request log (plan doc 10 §3.6, design 08 §8.4): the per-op breakdown from
    // ctx.spans is the first triage tool — it names the op that ate the time.
    if (auto thr = obs::slow_request_threshold_us(); thr != 0 && us >= thr) {
      std::string spans;
      for (uint32_t i = 0; i < ctx.span_count; ++i) {
        if (!spans.empty()) spans += ',';
        spans += std::format("{}={}us", op_name(ctx.spans[i].op), ctx.spans[i].us);
      }
      LNFS_WARN("v4 slow compound xid={:#x} peer={} dur={}us st={} ops={}/[{}]", call.xid,
                conn.peer.to_string(), us, status, ops_done, spans);
    }
  };

  xdr::XdrEnc enc(conn.pool);
  rpc::encode_reply_success(enc, call.xid);
  std::byte* status_gap = enc.raw_gap(4);
  enc.opaque(tag_bytes);
  std::byte* count_gap = enc.raw_gap(4);

  auto finish = [&](uint32_t status, uint32_t ops_done) -> rt::Task<void> {
    log_summary(status, ops_done);
    patch_u32(status_gap, status);
    patch_u32(count_gap, ops_done);
    co_await conn.send(enc.take());
  };

  if (!minor_supported(*minor)) {  // decision D5: minorversion 0 rejected; 1 and 2 served
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

  note_op(*first_op);
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
      if (dbg)
        LNFS_DEBUG("v4 xid={:#x} peer={} slot={} seq={} replay (cached compound)",
                   call.xid, conn.peer.to_string(), ctx.slotid, ctx.seqid);
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
    enc.u32(seq.status_flags);  // e.g. SEQ4_STATUS_CB_PATH_DOWN (plan doc 10 §5.2)
    status = st(Status::kOk);
    done = 1;
    bool saw_destroy_session = false;

    for (uint32_t i = 1; i < *numops && status == st(Status::kOk); ++i) {
      auto opcode = dec.u32();
      if (!opcode) {
        status = st(Status::kBadxdr);
        break;
      }
      note_op(*opcode);
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
      if (op == Op::kRead || op == Op::kReaddir || op == Op::kReadPlus) {
        // These budget themselves against ctx.max_response (clamp / truncate).
        status = co_await exec_op(ctx, *opcode, dec, enc);
        ++done;
        continue;
      }
      // Everything else encodes straight into the reply under a mark; an over-budget
      // result is rolled back and replaced by REP_TOO_BIG(_TO_CACHE) for exactly this
      // op (RFC 8881 §2.10.6.4). Replaces the stage-then-copy path (plan doc 10 §2.4).
      auto op_mark = enc.mark();
      status = co_await exec_op(ctx, *opcode, dec, enc);
      if (enc.size() + 16 > ctx.max_response) {
        status = ctx.cachethis && enc.size() + 16 <= seq.max_response
                     ? st(Status::kRepTooBigToCache)
                     : st(Status::kRepTooBig);
        enc.rollback(op_mark);
        enc.u32(*opcode);
        enc.u32(status);
        ++done;
        break;
      }
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
    log_summary(status, done);
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
  if (*first_op < kFirstOp || *first_op > last_op_for(ctx.minor)) {
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
  auto t0 = std::chrono::steady_clock::now();
  uint32_t status = co_await exec_op_impl(ctx, opcode, dec, enc);
  auto us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count());
  auto& m = obs::Metrics::instance();
  size_t idx = obs::v4_op_index(opcode);
  m.v4_op_names[idx].store(op_name(opcode), std::memory_order_relaxed);
  m.v4_op_calls[idx].fetch_add(1, std::memory_order_relaxed);
  if (status != 0) m.v4_op_errors[idx].fetch_add(1, std::memory_order_relaxed);
  m.v4_op_duration[idx].observe_us(us);
  if (ctx.span_count < Ctx::kMaxSpans)
    ctx.spans[ctx.span_count++] = {
        opcode, static_cast<uint32_t>(std::min<uint64_t>(us, UINT32_MAX))};
  co_return status;
}

rt::Task<uint32_t> Engine::exec_op_impl(Ctx& ctx, uint32_t opcode, xdr::XdrDec& dec,
                                        xdr::XdrEnc& enc) {
  switch (static_cast<Op>(opcode)) {
    case Op::kPutrootfh:
    case Op::kPutpubfh: {
      enc.u32(opcode);
      ctx.current_valid = false;
      co_return co_await op_putrootfh(ctx, enc);
    }
    case Op::kPutfh: {
      enc.u32(opcode);
      ctx.current_valid = false;
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
      ctx.saved_sid = ctx.current_sid;
      ctx.saved_valid = ctx.current_valid;
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
      ctx.current_sid = ctx.saved_sid;
      ctx.current_valid = ctx.saved_valid;
      enc.u32(st(Status::kOk));
      co_return st(Status::kOk);
    }
    case Op::kLookup: {
      enc.u32(opcode);
      ctx.current_valid = false;
      co_return co_await op_lookup(ctx, dec, enc);
    }
    case Op::kLookupp: {
      enc.u32(opcode);
      ctx.current_valid = false;
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
    case Op::kOpenDowngrade: {
      enc.u32(opcode);
      co_return co_await op_open_downgrade(ctx, dec, enc);
    }
    case Op::kDelegreturn: {
      enc.u32(opcode);
      co_return co_await op_delegreturn(ctx, dec, enc);
    }
    case Op::kWrite: {
      enc.u32(opcode);
      co_return co_await op_write(ctx, dec, enc);
    }
    case Op::kCommit: {
      enc.u32(opcode);
      co_return co_await op_commit(ctx, dec, enc);
    }
    case Op::kSetattr: {
      enc.u32(opcode);
      co_return co_await op_setattr(ctx, dec, enc);
    }
    case Op::kCreate: {
      enc.u32(opcode);
      ctx.current_valid = false;
      co_return co_await op_create(ctx, dec, enc);
    }
    case Op::kRemove: {
      enc.u32(opcode);
      co_return co_await op_remove(ctx, dec, enc);
    }
    case Op::kRename: {
      enc.u32(opcode);
      co_return co_await op_rename(ctx, dec, enc);
    }
    case Op::kLink: {
      enc.u32(opcode);
      co_return co_await op_link(ctx, dec, enc);
    }
    case Op::kLock: {
      enc.u32(opcode);
      co_return co_await op_lock(ctx, dec, enc);
    }
    case Op::kLockt: {
      enc.u32(opcode);
      co_return co_await op_lockt(ctx, dec, enc);
    }
    case Op::kLocku: {
      enc.u32(opcode);
      co_return co_await op_locku(ctx, dec, enc);
    }
    case Op::kSecinfo: {
      enc.u32(opcode);
      ctx.current_valid = false;
      co_return co_await op_secinfo(ctx, dec, enc);
    }
    case Op::kVerify:
    case Op::kNverify: {
      enc.u32(opcode);
      co_return co_await op_verify(ctx, dec, enc, static_cast<Op>(opcode) == Op::kNverify);
    }
    case Op::kSecinfoNoName: {
      enc.u32(opcode);
      ctx.current_valid = false;
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
    case Op::kSeek:
    case Op::kAllocate:
    case Op::kDeallocate:
    case Op::kCopy:
    case Op::kClone:
    case Op::kReadPlus: {
      if (ctx.minor < 2) break;  // 4.1 table ends at 58 -> OP_ILLEGAL below
      enc.u32(opcode);
      switch (static_cast<Op>(opcode)) {
        case Op::kSeek: co_return co_await op_seek(ctx, dec, enc);
        case Op::kAllocate: co_return co_await op_allocate(ctx, dec, enc, false);
        case Op::kDeallocate: co_return co_await op_allocate(ctx, dec, enc, true);
        case Op::kCopy: co_return co_await op_copy(ctx, dec, enc);
        case Op::kReadPlus: co_return co_await op_read_plus(ctx, dec, enc);
        default: co_return co_await op_clone(ctx, dec, enc);
      }
    }
    default: break;
  }
  // Inside the minor version's table but unimplemented -> NOTSUPP; beyond it -> ILLEGAL.
  if (opcode >= kFirstOp && opcode <= last_op_for(ctx.minor)) {
    enc.u32(opcode);
    enc.u32(st(Status::kNotsupp));
    co_return st(Status::kNotsupp);
  }
  enc.u32(static_cast<uint32_t>(Op::kIllegal));
  enc.u32(st(Status::kOpIllegal));
  co_return st(Status::kOpIllegal);
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
  if (name->empty() || !utf8_component(*name)) {
    // zero-length or malformed UTF-8 component: INVAL, not BADNAME (RFC 8881 §18.10)
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  if (!core::valid_component(*name)) {
    enc.u32(st(Status::kBadname));
    co_return st(Status::kBadname);
  }
  auto resolved = co_await resolve(ctx, ctx.cfh);
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
  auto resolved = co_await resolve(ctx, ctx.cfh);
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
  core::FsProps fs;
  backend::FsStats stats;
  if (resolved.pseudo()) {
    attr = pseudo_.attr_of(*resolved.node);
    src.fsid = 0;  // src.fs stays null: pseudo defaults
  } else {
    auto lock = locks_.get(resolved.exp->fsid, resolved.oid);
    auto held = co_await lock->lock_shared();
    auto got = co_await resolved.obj->getattr();
    if (!got) co_return st(core::to_v4(got.error(), Op::kGetattr));
    attr = *got;
    fs = core::fs_props(*resolved.exp->backend);
    src.fs = &fs;
    src.fsid = resolved.exp->fsid;
    src.lease_seconds = state_.config().lease_seconds;
    if (wants_stats(wanted)) {
      auto s = co_await resolved.exp->backend->statfs();
      if (s) {
        stats = *s;
        src.stats = &stats;
      }
    }
    if (wanted.test(attr::kMountedOnFileid)) {
      // Cached root oid (plan doc 10 §2.6): Linux clients request this attribute on
      // nearly every GETATTR/READDIR; the export-root comparison must not cost a
      // backend->root() (fd dup + handle encode) each time.
      auto root_oid = co_await root_oid_of(*resolved.exp);
      if (root_oid && *root_oid == resolved.oid) {
        if (auto* crossing = pseudo_.for_export(resolved.exp->fsid))
          src.mounted_on_fileid = crossing->id;
      }
    }
  }
  src.attr = &attr;
  src.fh = ctx.cfh;
  src.lease_seconds = state_.config().lease_seconds;
  enc.u32(st(Status::kOk));
  encode_fattr(enc, wanted, src);
  co_return st(Status::kOk);
}

rt::Task<Result<backend::ObjId>> Engine::root_oid_of(core::ExportEntry& exp) {
  {
    std::lock_guard g(root_oid_mu_);
    auto it = root_oids_.find(exp.fsid);
    if (it != root_oids_.end()) co_return it->second;
  }
  auto root_obj = co_await exp.backend->root();
  if (!root_obj) co_return Err(root_obj.error());
  backend::ObjId oid = (*root_obj)->id();
  std::lock_guard g(root_oid_mu_);
  root_oids_.emplace(exp.fsid, oid);
  co_return oid;
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
  auto resolved = co_await resolve(ctx, ctx.cfh);
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
  auto resolved = co_await resolve(ctx, ctx.cfh);
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
  auto resolved = co_await resolve(ctx, ctx.cfh);
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
  auto resolved = co_await resolve(ctx, ctx.cfh);
  if (!resolved) {
    uint32_t code = st(core::to_v4(resolved.error(), Op::kRead));
    enc.u32(code);
    co_return code;
  }
  if (resolved->pseudo()) {
    enc.u32(st(Status::kIsdir));
    co_return st(Status::kIsdir);
  }
  if (resolved->obj->type() == backend::FType::kDir) {
    enc.u32(st(Status::kIsdir));
    co_return st(Status::kIsdir);
  }
  if (resolved->obj->type() != backend::FType::kReg) {
    enc.u32(st(Status::kWrongType));
    co_return st(Status::kWrongType);
  }
  if (uint32_t cur = resolve_current(ctx, *sid); cur != st(Status::kOk)) {
    enc.u32(cur);
    co_return cur;
  }
  auto check = co_await state_.check_io(*sid, ctx.clientid, resolved->exp->fsid,
                                        resolved->oid, state::kShareRead);
  if (check.status != 0) {
    enc.u32(check.status);
    co_return check.status;
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
  // QoS (plan doc 10 §4.3): throttle before the object lock so a shaped client parks
  // on its own bucket, not on the file everyone else needs.
  co_await resolved->exp->qos.throttle(false, len);
  if (auto* cq = client_qos(ctx.clientid)) {
    co_await cq->ops.acquire(1);
    co_await cq->read_bytes.acquire(len);
  }
  auto data = ctx.conn.pool.alloc(std::max<uint32_t>(len, 1));
  bool eof = false;
  auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
  auto held = co_await lock->lock_shared();
  backend::OpenCtx open{cred, check.bopen.get()};
  auto n = co_await resolved->obj->read(open, *offset,
                                        std::span<std::byte>(data.data(), len), eof);
  if (!n) {
    uint32_t code = st(core::to_v4(n.error(), Op::kRead));
    enc.u32(code);
    co_return code;
  }
  obs::Metrics::instance().read_bytes.fetch_add(*n, std::memory_order_relaxed);
  resolved->exp->metrics.read_bytes.fetch_add(*n, std::memory_order_relaxed);
  resolved->exp->metrics.read_ops.fetch_add(1, std::memory_order_relaxed);
  enc.u32(st(Status::kOk));
  enc.boolean(eof);
  enc.u32(*n);
  enc.attach(std::move(data), 0, *n);
  co_return st(Status::kOk);
}

// READ_PLUS (RFC 7862 §15.10, plan doc 10 §5.1): sparse-aware READ — the reply is a
// list of DATA/HOLE segments.  This implementation returns at most one segment per
// call: the hole the offset sits in (clamped to the request and EOF), or one data
// extent read the normal way and clamped to the next hole.  Short replies are the
// protocol's norm; the client continues from wherever the last segment ended.
// Backends without kSparseOps degrade to a single DATA segment (plain READ shape).
rt::Task<uint32_t> Engine::op_read_plus(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
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
  auto resolved = co_await resolve(ctx, ctx.cfh);
  if (!resolved) {
    uint32_t code = st(core::to_v4(resolved.error(), Op::kReadPlus));
    enc.u32(code);
    co_return code;
  }
  if (resolved->pseudo() || resolved->obj->type() == backend::FType::kDir) {
    enc.u32(st(Status::kIsdir));
    co_return st(Status::kIsdir);
  }
  if (resolved->obj->type() != backend::FType::kReg) {
    enc.u32(st(Status::kWrongType));
    co_return st(Status::kWrongType);
  }
  if (uint32_t cur = resolve_current(ctx, *sid); cur != st(Status::kOk)) {
    enc.u32(cur);
    co_return cur;
  }
  auto check = co_await state_.check_io(*sid, ctx.clientid, resolved->exp->fsid,
                                        resolved->oid, state::kShareRead);
  if (check.status != 0) {
    enc.u32(check.status);
    co_return check.status;
  }
  auto mapped = exports_.squash_cred(ctx.cred, *resolved->exp);
  auto cred = mapped.view();
  size_t slack = 256;  // op headers + eof/segment framing
  size_t budget = ctx.max_response > enc.size() + slack
                      ? ctx.max_response - enc.size() - slack
                      : 0;
  uint32_t len = std::min<uint32_t>(
      {*count, resolved->exp->backend->limits().max_read,
       static_cast<uint32_t>(std::min<size_t>(budget, UINT32_MAX))});
  co_await resolved->exp->qos.throttle(false, len);
  if (auto* cq = client_qos(ctx.clientid)) {
    co_await cq->ops.acquire(1);
    co_await cq->read_bytes.acquire(len);
  }
  auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
  auto held = co_await lock->lock_shared();
  backend::OpenCtx open{cred, check.bopen.get()};

  auto attr = co_await resolved->obj->getattr();
  if (!attr) {
    uint32_t code = st(core::to_v4(attr.error(), Op::kReadPlus));
    enc.u32(code);
    co_return code;
  }
  if (*offset >= attr->size || len == 0) {  // nothing at/after the offset
    enc.u32(st(Status::kOk));
    enc.boolean(*offset >= attr->size);
    enc.u32(0);
    co_return st(Status::kOk);
  }

  // Where does data begin at/after the offset?  ENXIO = a hole runs to EOF.
  uint64_t data_at = *offset;
  bool sparse = resolved->exp->backend->caps().has(backend::Cap::kSparseOps);
  if (sparse) {
    auto found = co_await resolved->obj->seek(open, *offset, backend::SeekWhat::kData);
    if (found) data_at = std::max(*found, *offset);
    else if (found.error() == errno_from(ENXIO)) data_at = attr->size;
    else sparse = false;  // sparse probe unavailable: serve everything as data
  }
  if (data_at > *offset) {  // the offset sits in a hole
    uint64_t hole_end = std::min<uint64_t>({data_at, *offset + *count, attr->size});
    enc.u32(st(Status::kOk));
    enc.boolean(hole_end >= attr->size);
    enc.u32(1);
    enc.u32(kContentHole);
    enc.u64(*offset);
    enc.u64(hole_end - *offset);
    co_return st(Status::kOk);
  }

  // Data at the offset: clamp the read to the next hole so the segment stays pure.
  if (sparse) {
    auto hole = co_await resolved->obj->seek(open, *offset, backend::SeekWhat::kHole);
    if (hole && *hole > *offset)
      len = static_cast<uint32_t>(std::min<uint64_t>(len, *hole - *offset));
  }
  auto data = ctx.conn.pool.alloc(std::max<uint32_t>(len, 1));
  bool eof = false;
  auto n = co_await resolved->obj->read(open, *offset,
                                        std::span<std::byte>(data.data(), len), eof);
  if (!n) {
    uint32_t code = st(core::to_v4(n.error(), Op::kReadPlus));
    enc.u32(code);
    co_return code;
  }
  obs::Metrics::instance().read_bytes.fetch_add(*n, std::memory_order_relaxed);
  resolved->exp->metrics.read_bytes.fetch_add(*n, std::memory_order_relaxed);
  resolved->exp->metrics.read_ops.fetch_add(1, std::memory_order_relaxed);
  // A data segment ending at a mid-file hole is not EOF even if the read came short.
  bool at_eof = *offset + *n >= attr->size;
  enc.u32(st(Status::kOk));
  enc.boolean(eof && at_eof);
  if (*n == 0) {
    enc.u32(0);
    co_return st(Status::kOk);
  }
  enc.u32(1);
  enc.u32(kContentData);
  enc.u64(*offset);
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
  auto resolved = co_await resolve(ctx, ctx.cfh);
  if (!resolved) {
    uint32_t code = st(core::to_v4(resolved.error(), Op::kReaddir));
    enc.u32(code);
    co_return code;
  }

  // Semantic cookie verifier (plan doc 10 §5.1): derived from the directory change
  // attribute.  A directory modified between pages flips the verifier; the client
  // gets NOT_SAME and restarts the listing instead of silently duplicating or
  // missing entries against stale cookies.
  uint64_t dir_change = 0;
  if (resolved->pseudo()) {
    dir_change = pseudo_.attr_of(*resolved->node).change;
  } else {
    auto dattr = co_await resolved->obj->getattr();
    if (!dattr) {
      uint32_t code = st(core::to_v4(dattr.error(), Op::kReaddir));
      enc.u32(code);
      co_return code;
    }
    dir_change = dattr->change;
  }
  std::array<std::byte, 8> dir_verf{};
  std::memcpy(dir_verf.data(), &dir_change, sizeof(dir_change));
  if (*cookie != 0 &&
      !std::equal(verf->begin(), verf->end(), dir_verf.begin(), dir_verf.end())) {
    enc.u32(st(Status::kNotSame));
    co_return st(Status::kNotSame);
  }

  size_t slack = 256;
  size_t budget = ctx.max_response > enc.size() + slack
                      ? std::min<size_t>(*maxcount, ctx.max_response - enc.size() - slack)
                      : 0;
  if (budget < 128) {
    enc.u32(st(Status::kToosmall));
    co_return st(Status::kToosmall);
  }
  // Per-page backend batch sized from the client's dircount budget like the v3 path
  // (plan doc 10 §5.1) — the hardcoded 128 made big-directory listings pay dozens of
  // extra backend round-trips per reply.
  uint32_t batch = std::min<uint32_t>(4096, std::max<uint32_t>(16, *dircount / 24));

  // Export side: fetch the first page before committing to an OK status, so cursor
  // errors (BAD_COOKIE) can still be reported cleanly.
  backend::DirPage first_page;
  core::MappedCred mapped;
  backend::Cred cred{};
  core::FsProps fs;
  std::shared_ptr<rt::AsyncSharedMutex> lock;
  if (!resolved->pseudo()) {
    mapped = exports_.squash_cred(ctx.cred, *resolved->exp);
    cred = mapped.view();
    fs = core::fs_props(*resolved->exp->backend);
    lock = locks_.get(resolved->exp->fsid, resolved->oid);
    auto held = co_await lock->lock_shared();
    auto page = co_await core::readdir_page(resolved->obj, cred, *cookie, batch);
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
  enc.opaque_fixed(dir_verf);
  size_t used = 32;       // verifier + list/eof framing
  size_t used_dir = 0;    // dircount budget: cookie+name portions
  bool truncated = false;
  bool eof = false;

  auto emit = [&](uint64_t entry_cookie, std::string_view name,
                  const backend::Attr& attr, const FhBytes& fh, uint64_t fsid_val,
                  uint64_t mounted_on, const core::FsProps* ent_fs) -> bool {
    size_t name_part = 8 + 4 + ((name.size() + 3) & ~size_t(3));
    if (used_dir + name_part > *dircount) {
      truncated = true;
      return false;
    }
    // Encode the entry in place under a mark; over-budget rolls it back (plan doc 10
    // §2.4 — was one staging XdrEnc + flatten + copy per entry).
    auto entry_mark = enc.mark();
    const size_t entry_start = enc.size();
    enc.boolean(true);
    enc.u64(entry_cookie);
    enc.string(name);
    AttrSource src;
    src.attr = &attr;
    src.fsid = fsid_val;
    src.mounted_on_fileid = mounted_on;
    src.fh = fh;
    src.fs = ent_fs;
    src.lease_seconds = state_.config().lease_seconds;
    encode_fattr(enc, *wanted, src);
    const size_t entry_size = enc.size() - entry_start;
    if (used + entry_size > budget) {
      enc.rollback(entry_mark);
      truncated = true;
      return false;
    }
    used += entry_size;
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
        core::FsProps child_fs = core::fs_props(*child->exp->backend);
        ok = emit(this_cookie, name, *attr, export_fh(*child->exp, (*obj)->id()),
                  child->exp->fsid, child->id, &child_fs);
      } else {
        auto attr = pseudo_.attr_of(*child);
        ok = emit(this_cookie, name, attr, pseudo_fh(*child), 0, child->id, nullptr);
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
                  resolved->exp->fsid, attr.fileid, &fs))
          break;
      }
      if (truncated) break;
      if (page.eof) {
        eof = true;
        break;
      }
      auto next = co_await core::readdir_page(resolved->obj, cred, next_cookie, batch);
      if (!next) break;  // mid-stream error: return what we have, eof=false
      page = std::move(*next);
    }
  }
  enc.boolean(false);  // end of entries
  enc.boolean(eof && !truncated);
  co_return st(Status::kOk);
}

// ---- open state ops (phase 4: design 07 §6.1) -------------------------------

namespace {

// v4 createmode4 / opentype4 / open_claim_type4 / nf4type wire values.
constexpr uint32_t kOpenNocreate = 0, kOpenCreate = 1;
constexpr uint32_t kCreateUnchecked = 0, kCreateGuarded = 1, kCreateExclusive = 2,
                   kCreateExclusive41 = 3;
constexpr uint32_t kClaimNull = 0, kClaimPrevious = 1, kClaimDelegateCur = 2,
                   kClaimDelegatePrev = 3, kClaimFh = 4, kClaimDelegCurFh = 5,
                   kClaimDelegPrevFh = 6;
[[maybe_unused]] constexpr uint32_t kNf4Reg = 1;  // CREATE rejects REG (OPEN creates files)
constexpr uint32_t kNf4Dir = 2, kNf4Blk = 3, kNf4Chr = 4, kNf4Lnk = 5, kNf4Sock = 6,
                   kNf4Fifo = 7;
constexpr uint32_t kOpenResultLocktypePosix = 0x4;

void encode_change_info(xdr::XdrEnc& enc, bool atomic, uint64_t before, uint64_t after) {
  enc.boolean(atomic);
  enc.u64(before);
  enc.u64(after);
}

uint64_t change_of(const Result<backend::Attr>& attr) {
  return attr ? attr->change : 0;
}

}  // namespace

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
  // openhow
  uint32_t create_mode = kCreateUnchecked;
  bool create = false;
  backend::SetAttr create_attrs;
  Bitmap attrset;
  backend::ExclVerf excl_verf{};
  if (*opentype == kOpenCreate) {
    create = true;
    auto mode = dec.u32();
    if (!mode) {
      enc.u32(st(Status::kBadxdr));
      co_return st(Status::kBadxdr);
    }
    create_mode = *mode;
    if (create_mode == kCreateExclusive || create_mode == kCreateExclusive41) {
      auto verf = dec.opaque_fixed(8);
      if (!verf) {
        enc.u32(st(Status::kBadxdr));
        co_return st(Status::kBadxdr);
      }
      std::copy(verf->begin(), verf->end(), excl_verf.begin());
    }
    if (create_mode == kCreateUnchecked || create_mode == kCreateGuarded ||
        create_mode == kCreateExclusive41) {
      Status code = decode_settable_fattr(dec, create_attrs, attrset);
      if (code != Status::kOk) {
        enc.u32(st(code));
        co_return st(code);
      }
    }
    if (create_mode > kCreateExclusive41) {
      enc.u32(st(Status::kInval));
      co_return st(Status::kInval);
    }
  } else if (*opentype != kOpenNocreate) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  // claim
  auto claim = dec.u32();
  if (!claim) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  std::string name;
  bool deleg_claim = false;
  Stateid deleg_claim_sid{};
  switch (*claim) {
    case kClaimNull: {
      auto n = dec.string(kMaxName + 1);
      if (!n) {
        enc.u32(st(Status::kBadxdr));
        co_return st(Status::kBadxdr);
      }
      name = *n;
      break;
    }
    case kClaimPrevious: {
      auto deleg = dec.u32();  // delegate_type: no delegations are ever granted
      if (!deleg) {
        enc.u32(st(Status::kBadxdr));
        co_return st(Status::kBadxdr);
      }
      break;
    }
    case kClaimFh: break;
    case kClaimDelegCurFh: {
      // Recall response (plan doc 10 §5.2): the client converts opens it holds under
      // a delegation being recalled into real open stateids before DELEGRETURN.
      auto dsid = Stateid::decode(dec);
      if (!dsid) {
        enc.u32(st(Status::kBadxdr));
        co_return st(Status::kBadxdr);
      }
      deleg_claim_sid = *dsid;
      deleg_claim = true;
      break;
    }
    case kClaimDelegateCur:
    case kClaimDelegatePrev:
    case kClaimDelegPrevFh: {
      enc.u32(st(Status::kNotsupp));  // 4.0-style / reboot delegation claims
      co_return st(Status::kNotsupp);
    }
    default: {
      enc.u32(st(Status::kInval));
      co_return st(Status::kInval);
    }
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
  uint32_t deny = *share_deny;
  if (access == 0 || deny > state::kShareBoth) {
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  // open_owner4.clientid is ignored in 4.1 (RFC 8881 §18.16.3): the session says who.
  std::string owner_bytes(reinterpret_cast<const char*>(owner->data()), owner->size());

  auto dir = co_await resolve(ctx, ctx.cfh);
  if (!dir) {
    uint32_t code = st(core::to_v4(dir.error(), Op::kOpen));
    enc.u32(code);
    co_return code;
  }

  backend::ObjPtr file;
  bool created_new = false;  // skip the permission check the creator implicitly passed
  bool atomic = false;
  uint64_t change_before = 0, change_after = 0;
  core::ExportEntry* exp = dir->exp;

  if (*claim == kClaimNull) {
    if (name.empty()) {
      enc.u32(st(Status::kInval));
      co_return st(Status::kInval);
    }
    if (!utf8_component(name)) {
      enc.u32(st(Status::kInval));
      co_return st(Status::kInval);
    }
    if (!core::valid_component(name)) {
      enc.u32(st(Status::kBadname));
      co_return st(Status::kBadname);
    }
    if (dir->pseudo()) {  // the synthesized tree holds only directories
      if (create) {
        enc.u32(st(Status::kRofs));
        co_return st(Status::kRofs);
      }
      uint32_t code = dir->node->children.contains(name) ? st(Status::kIsdir)
                                                         : st(Status::kNoent);
      enc.u32(code);
      co_return code;
    }
    if (dir->obj->type() != backend::FType::kDir) {
      enc.u32(st(Status::kNotdir));
      co_return st(Status::kNotdir);
    }
    if (!create) {
      auto mapped = exports_.squash_cred(ctx.cred, *exp);
      auto cred = mapped.view();
      auto lock = locks_.get(exp->fsid, dir->oid);
      auto held = co_await lock->lock_shared();
      auto before = co_await dir->obj->getattr();
      auto found = co_await dir->obj->lookup(cred, name);
      auto after = co_await dir->obj->getattr();
      change_before = change_of(before);
      change_after = change_of(after);
      if (!found) {
        uint32_t code = st(core::to_v4(found.error(), Op::kOpen));
        enc.u32(code);
        co_return code;
      }
      file = std::move(*found);
    } else {
      MutateGuard guard(locks_, exports_, *exp, ctx.cred);
      if (auto verdict = guard.precheck({}); !verdict) {
        uint32_t code = verdict_status4(verdict);
        enc.u32(code);
        co_return code;
      }
      co_await guard.enter({dir->obj, dir->oid});  // exclusive: change_info is atomic
      atomic = true;
      const backend::Cred& cred = guard.cred();
      Result<backend::Created> made = Err(errno_from(EIO));
      if (create_mode == kCreateExclusive || create_mode == kCreateExclusive41) {
        made = co_await dir->obj->create(cred, name, {}, &excl_verf);
        if (made && create_mode == kCreateExclusive41) {
          // EXCLUSIVE4_1: the verifier lives in the timestamps, so only the other
          // attributes are applied; attrset tells the client which ones were used.
          backend::SetAttr rest = create_attrs;
          rest.atime_how = backend::SetAttr::TimeHow::kOmit;
          rest.mtime_how = backend::SetAttr::TimeHow::kOmit;
          rest.size.reset();
          if (rest.mode || rest.uid || rest.gid) {
            auto set = co_await made->obj->setattr(cred, rest);
            if (set) made->attr = *set;
          }
          Bitmap used;
          if (rest.mode) used.set(attr::kMode);
          if (rest.uid) used.set(attr::kOwner);
          if (rest.gid) used.set(attr::kOwnerGroup);
          attrset = used;
        } else {
          attrset = Bitmap{};
        }
        if (made) {
          attrset.set(attr::kTimeAccess);  // verifier storage: client must not reset
          attrset.set(attr::kTimeModify);
        }
        created_new = made.has_value();
      } else {
        made = co_await dir->obj->create(cred, name, create_attrs, nullptr);
        created_new = made.has_value();
        if (!made && made.error() == errno_from(EEXIST) &&
            create_mode == kCreateUnchecked) {
          // UNCHECKED on an existing file succeeds; only a requested size applies.
          auto existing = co_await dir->obj->lookup(cred, name);
          if (existing) {
            if ((*existing)->type() == backend::FType::kDir) {
              enc.u32(st(Status::kIsdir));
              co_return st(Status::kIsdir);
            }
            backend::Attr attr{};
            if (create_attrs.size && (*existing)->type() == backend::FType::kReg) {
              backend::SetAttr size_only;
              size_only.size = create_attrs.size;
              auto set = co_await (*existing)->setattr(cred, size_only);
              if (!set) {
                uint32_t code = st(core::to_v4(set.error(), Op::kOpen));
                enc.u32(code);
                co_return code;
              }
              attr = *set;
              attrset = Bitmap{};
              attrset.set(attr::kSize);
            } else {
              attrset = Bitmap{};
            }
            made = backend::Created{*existing, attr};
          }
        }
      }
      co_await guard.finish();
      change_before = guard.first().change_before();
      change_after = guard.first().change_after();
      if (!made) {
        uint32_t code = st(core::to_v4(made.error(), Op::kOpen));
        enc.u32(code);
        co_return code;
      }
      file = std::move(made->obj);
    }
  } else {  // CLAIM_FH / CLAIM_PREVIOUS / CLAIM_DELEG_CUR_FH: CFH is the file itself
    if (dir->pseudo()) {
      enc.u32(st(Status::kIsdir));
      co_return st(Status::kIsdir);
    }
    file = dir->obj;
  }
  if (deleg_claim) {
    // The claimed stateid must be this client's delegation on this very file.
    uint32_t code = co_await state_.check_deleg_claim(deleg_claim_sid, ctx.clientid,
                                                      exp->fsid, file->id());
    if (code != 0) {
      enc.u32(code);
      co_return code;
    }
  }

  if (file->type() == backend::FType::kDir) {
    enc.u32(st(Status::kIsdir));
    co_return st(Status::kIsdir);
  }
  if (file->type() == backend::FType::kLnk) {
    enc.u32(st(Status::kSymlink));
    co_return st(Status::kSymlink);
  }
  if (file->type() != backend::FType::kReg) {
    enc.u32(st(Status::kWrongType));  // devices/fifos/sockets are not OPENable here
    co_return st(Status::kWrongType);
  }
  if ((access & state::kShareWrite) && exp->readonly) {
    enc.u32(st(Status::kRofs));
    co_return st(Status::kRofs);
  }
  auto mapped = exports_.squash_cred(ctx.cred, *exp);
  auto cred = mapped.view();
  // POSIX permission check for the requested modes.  kNativeAccess backends (plan
  // doc 10 §5.3) authorize the open() below themselves under the caller's identity
  // (storage-side ACLs, no EOPNOTSUPP degrade), so the pre-flight — one more storage
  // round trip — is skipped for them.
  if (!created_new && !exp->backend->caps().has(backend::Cap::kNativeAccess)) {
    backend::AccessMask want;
    if (access & state::kShareRead) want.set(backend::Access::kRead);
    if (access & state::kShareWrite) want.set(backend::Access::kModify);
    auto lock = locks_.get(exp->fsid, file->id());
    auto held = co_await lock->lock_shared();
    auto allowed = co_await file->access(cred, want);
    if (!allowed) {
      uint32_t code = st(core::to_v4(allowed.error(), Op::kOpen));
      enc.u32(code);
      co_return code;
    }
    if (((access & state::kShareRead) && !allowed->has(backend::Access::kRead)) ||
        ((access & state::kShareWrite) && !allowed->has(backend::Access::kModify))) {
      enc.u32(st(Status::kAccess));
      co_return st(Status::kAccess);
    }
  }
  // Backend open handle (design 05): optional; backends without open state say so.
  backend::OpenFlags oflags;
  if (access & state::kShareRead) oflags.set(backend::OpenFlag::kRead);
  if (access & state::kShareWrite) oflags.set(backend::OpenFlag::kWrite);
  backend::OpenPtr bopen;
  {
    auto opened = co_await file->open(cred, oflags);
    if (opened) bopen = std::move(*opened);
    else if (opened.error() != errno_from(EOPNOTSUPP)) {
      uint32_t code = st(core::to_v4(opened.error(), Op::kOpen));
      enc.u32(code);
      co_return code;
    }
  }

  state::StateMgr::OpenArgs args;
  args.clientid = ctx.clientid;
  args.fsid = exp->fsid;
  args.oid = file->id();
  args.owner = std::move(owner_bytes);
  args.access = access;
  args.deny = deny;
  const bool is_reclaim = *claim == kClaimPrevious;
  args.reclaim = is_reclaim;
  args.deleg_claim = deleg_claim;
  auto opened = co_await state_.open(std::move(args), std::move(bopen));
  if (opened.status != 0) {
    enc.u32(opened.status);
    co_return opened.status;
  }
  ctx.cfh = export_fh(*exp, file->id());
  ctx.current_sid = opened.stateid;
  ctx.current_valid = true;
  // Read delegation grant (plan doc 10 §5.2): policy lives in the state manager; a
  // WANT_NO_DELEG / WANT_CANCEL request and the recall-response claim never grant.
  uint32_t want = *share_access & 0xFF00;
  state::StateMgr::DelegGrant grant;
  if (!deleg_claim && want != 0x0400 && want != 0x0500) {
    grant = co_await state_.maybe_grant_read_deleg(ctx.sessionid, ctx.clientid,
                                                   exp->fsid, file->id(), ctx.cfh,
                                                   access, is_reclaim);
  }
  enc.u32(st(Status::kOk));
  opened.stateid.encode(enc);
  encode_change_info(enc, atomic, change_before, change_after);
  enc.u32(kOpenResultLocktypePosix);
  attrset.encode(enc);
  if (grant.granted) {
    enc.u32(1);  // OPEN_DELEGATE_READ
    grant.stateid.encode(enc);
    enc.boolean(false);  // recall
    // nfsace4: a null everyone-ACE — no extra permissions ride the delegation.
    enc.u32(0);  // ACCESS_ALLOWED_ACE_TYPE
    enc.u32(0);  // flags
    enc.u32(0);  // access mask
    enc.string("EVERYONE@");
  } else if (want == 0) {
    enc.u32(0);  // OPEN_DELEGATE_NONE
  } else {
    enc.u32(3);  // OPEN_DELEGATE_NONE_EXT (RFC 8881 §18.16.3)
    if (want == 0x0400 || want == 0x0500) {
      enc.u32(0);  // WND4_NOT_WANTED (WANT_NO_DELEG / WANT_CANCEL)
    } else {
      enc.u32(2);          // WND4_RESOURCE
      enc.boolean(false);  // will_signal_deleg_avail
    }
  }
  co_return st(Status::kOk);
}

// DELEGRETURN (RFC 8881 §18.6, plan doc 10 §5.2): the client hands a delegation
// back — voluntarily or as its answer to CB_RECALL.
rt::Task<uint32_t> Engine::op_delegreturn(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto sid = Stateid::decode(dec);
  if (!sid) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  auto resolved = co_await resolve(ctx, ctx.cfh);
  if (!resolved) {
    uint32_t code = st(core::to_v4(resolved.error(), Op::kDelegreturn));
    enc.u32(code);
    co_return code;
  }
  if (resolved->pseudo()) {
    enc.u32(st(Status::kBadStateid));
    co_return st(Status::kBadStateid);
  }
  uint32_t code = co_await state_.delegreturn(*sid, ctx.clientid, resolved->exp->fsid,
                                              resolved->oid);
  enc.u32(code);
  co_return code;
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
  if (uint32_t cur = resolve_current(ctx, *sid); cur != st(Status::kOk)) {
    enc.u32(cur);
    co_return cur;
  }
  Stateid out;
  uint32_t code = co_await state_.close_state(*sid, ctx.clientid, &out);
  if (code != 0) {
    enc.u32(code);
    co_return code;
  }
  ctx.current_sid = out;
  ctx.current_valid = true;
  enc.u32(st(Status::kOk));
  out.encode(enc);
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_open_downgrade(Ctx& ctx, xdr::XdrDec& dec,
                                             xdr::XdrEnc& enc) {
  auto sid = Stateid::decode(dec);
  auto seqid = dec.u32();
  auto access = dec.u32();
  auto deny = dec.u32();
  if (!sid || !seqid || !access || !deny) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  if ((*access & 0x3) == 0 || (*access & ~0x3u) != 0 || *deny > state::kShareBoth) {
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  if (uint32_t cur = resolve_current(ctx, *sid); cur != st(Status::kOk)) {
    enc.u32(cur);
    co_return cur;
  }
  Stateid out;
  uint32_t code = co_await state_.open_downgrade(*sid, ctx.clientid, *access, *deny, &out);
  if (code != 0) {
    enc.u32(code);
    co_return code;
  }
  ctx.current_sid = out;
  ctx.current_valid = true;
  enc.u32(st(Status::kOk));
  out.encode(enc);
  co_return st(Status::kOk);
}

// ---- write-side IO ---------------------------------------------------------

rt::Task<uint32_t> Engine::op_write(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto sid = Stateid::decode(dec);
  auto offset = dec.u64();
  auto stable = dec.u32();
  // Zero-copy segment views of the payload (plan doc 10 §2.4): no gather-copy even
  // when the opaque spans recv buffers.
  SmallVec<std::span<const std::byte>, 8> data_segs;
  auto data_len = dec.opaque_spans(UINT32_MAX, data_segs);
  if (!sid || !offset || !stable || !data_len) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  if (*stable > 2) {
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  auto resolved = co_await resolve(ctx, ctx.cfh);
  if (!resolved) {
    uint32_t code = st(core::to_v4(resolved.error(), Op::kWrite));
    enc.u32(code);
    co_return code;
  }
  if (resolved->pseudo()) {
    enc.u32(st(Status::kRofs));
    co_return st(Status::kRofs);
  }
  if (resolved->obj->type() == backend::FType::kDir) {
    enc.u32(st(Status::kIsdir));
    co_return st(Status::kIsdir);
  }
  if (resolved->obj->type() != backend::FType::kReg) {
    enc.u32(st(Status::kWrongType));
    co_return st(Status::kWrongType);
  }
  // Stateid discipline first (special → table → OPENMODE), then the same backend call
  // the v3 path makes.
  if (uint32_t cur = resolve_current(ctx, *sid); cur != st(Status::kOk)) {
    enc.u32(cur);
    co_return cur;
  }
  auto check = co_await state_.check_io(*sid, ctx.clientid, resolved->exp->fsid,
                                        resolved->oid, state::kShareWrite);
  if (check.status != 0) {
    enc.u32(check.status);
    co_return check.status;
  }
  MutateGuard guard(locks_, exports_, *resolved->exp, ctx.cred);
  if (auto verdict = guard.precheck({}); !verdict) {
    uint32_t code = verdict_status4(verdict);
    enc.u32(code);
    co_return code;
  }
  if (*offset + *data_len < *offset) {  // offset+length overflow
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  uint32_t count = static_cast<uint32_t>(
      std::min<size_t>(*data_len, resolved->exp->backend->limits().max_write));
  SmallVec<iovec, 8> iov;
  for (uint32_t left = count; const auto& seg : data_segs) {
    if (left == 0) break;
    uint32_t k = static_cast<uint32_t>(std::min<size_t>(left, seg.size()));
    iov.push_back(iovec{const_cast<std::byte*>(seg.data()), k});
    left -= k;
  }
  backend::Stability stability = *stable == 0   ? backend::Stability::kUnstable
                                 : *stable == 1 ? backend::Stability::kDataSync
                                                : backend::Stability::kFileSync;
  // QoS (plan doc 10 §4.3): throttle before the exclusive object lock.
  co_await resolved->exp->qos.throttle(true, count);
  if (auto* cq = client_qos(ctx.clientid)) {
    co_await cq->ops.acquire(1);
    co_await cq->write_bytes.acquire(count);
  }
  co_await guard.enter({resolved->obj, resolved->oid, /*sample=*/false});
  backend::OpenCtx open{guard.cred(), check.bopen.get()};
  auto written = co_await resolved->obj->write(
      open, *offset, std::span<const iovec>(iov.data(), iov.size()), stability);
  if (!written) {
    uint32_t code = st(core::to_v4(written.error(), Op::kWrite));
    enc.u32(code);
    co_return code;
  }
  obs::Metrics::instance().write_bytes.fetch_add(*written, std::memory_order_relaxed);
  resolved->exp->metrics.write_bytes.fetch_add(*written, std::memory_order_relaxed);
  resolved->exp->metrics.write_ops.fetch_add(1, std::memory_order_relaxed);
  enc.u32(st(Status::kOk));
  enc.u32(*written);
  enc.u32(*stable);  // the backend honored the requested stability exactly
  enc.opaque_fixed(write_verf_);
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_commit(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto offset = dec.u64();
  auto count = dec.u32();
  if (!offset || !count) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  auto resolved = co_await resolve(ctx, ctx.cfh);
  if (!resolved) {
    uint32_t code = st(core::to_v4(resolved.error(), Op::kCommit));
    enc.u32(code);
    co_return code;
  }
  if (resolved->pseudo() || resolved->obj->type() == backend::FType::kDir) {
    enc.u32(st(Status::kIsdir));
    co_return st(Status::kIsdir);
  }
  if (resolved->obj->type() != backend::FType::kReg) {
    enc.u32(st(Status::kWrongType));
    co_return st(Status::kWrongType);
  }
  auto mapped = exports_.squash_cred(ctx.cred, *resolved->exp);
  auto cred = mapped.view();
  auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
  auto held = co_await lock->lock_shared();  // flushing does not mutate
  backend::OpenCtx open{cred, nullptr};
  auto committed = co_await resolved->obj->commit(open, *offset, *count);
  if (!committed) {
    uint32_t code = st(core::to_v4(committed.error(), Op::kCommit));
    enc.u32(code);
    co_return code;
  }
  enc.u32(st(Status::kOk));
  enc.opaque_fixed(write_verf_);
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_setattr(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto sid = Stateid::decode(dec);
  if (!sid) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  backend::SetAttr attrs;
  Bitmap wanted;
  Status decoded = decode_settable_fattr(dec, attrs, wanted);
  // SETATTR4res always carries attrsset (empty on failure).
  auto fail = [&](uint32_t code) {
    enc.u32(code);
    Bitmap none;
    none.encode(enc);
    return code;
  };
  if (decoded != Status::kOk) co_return fail(st(decoded));
  if (ctx.cfh.empty()) co_return fail(st(Status::kNofilehandle));
  auto resolved = co_await resolve(ctx, ctx.cfh);
  if (!resolved) co_return fail(st(core::to_v4(resolved.error(), Op::kSetattr)));
  if (resolved->pseudo()) co_return fail(st(Status::kRofs));
  if (attrs.size) {
    if (resolved->obj->type() == backend::FType::kDir) co_return fail(st(Status::kIsdir));
    if (resolved->obj->type() != backend::FType::kReg)
      co_return fail(st(Status::kWrongType));
    // Truncation is a write: the stateid must grant WRITE (or be special).
    if (uint32_t cur = resolve_current(ctx, *sid); cur != st(Status::kOk))
      co_return fail(cur);
    auto check = co_await state_.check_io(*sid, ctx.clientid, resolved->exp->fsid,
                                          resolved->oid, state::kShareWrite);
    if (check.status != 0) co_return fail(check.status);
  }
  MutateGuard guard(locks_, exports_, *resolved->exp, ctx.cred);
  if (auto verdict = guard.precheck({}); !verdict)
    co_return fail(verdict_status4(verdict));
  // Attribute changes invalidate read delegations (plan doc 10 §5.2): recall + DELAY.
  if (uint32_t dc = co_await state_.deleg_conflict(resolved->exp->fsid, resolved->oid);
      dc != 0)
    co_return fail(dc);
  co_await guard.enter({resolved->obj, resolved->oid, /*sample=*/false});
  auto result = co_await resolved->obj->setattr(guard.cred(), attrs);
  if (!result) co_return fail(st(core::to_v4(result.error(), Op::kSetattr)));
  enc.u32(st(Status::kOk));
  wanted.encode(enc);
  co_return st(Status::kOk);
}

// VERIFY / NVERIFY (RFC 8881 §18.31/§18.15): the client's fattr4 is compared with
// the server's encoding of the same mask.  Byte comparison is exact because attrlist4
// encoding is canonical for every attribute we support.
rt::Task<uint32_t> Engine::op_verify(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc,
                                     bool nverify) {
  auto mask = Bitmap::decode(dec);
  auto vals = mask ? dec.opaque(1u << 20) : Result<std::span<const std::byte>>(Err(Errno::kGarbage));
  if (!mask || !vals) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  const Bitmap& sup = supported_attrs();
  for (uint32_t bit = 0; bit < 96; ++bit) {
    if (!mask->test(bit)) continue;
    if (bit == attr::kRdattrError) {  // never meaningful in a VERIFY
      enc.u32(st(Status::kInval));
      co_return st(Status::kInval);
    }
    if (!sup.test(bit)) {
      enc.u32(st(Status::kAttrnotsupp));
      co_return st(Status::kAttrnotsupp);
    }
  }
  auto resolved = co_await resolve(ctx, ctx.cfh);
  if (!resolved) {
    uint32_t code = st(core::to_v4(resolved.error(), Op::kGetattr));
    enc.u32(code);
    co_return code;
  }
  xdr::XdrEnc staged(ctx.conn.pool);
  uint32_t code = co_await attr_reply(ctx, *resolved, *mask, staged);
  if (code != st(Status::kOk)) {
    enc.u32(code);
    co_return code;
  }
  auto bytes = staged.take().to_bytes();
  xdr::XdrDec mine(std::span<const std::byte>(bytes.data(), bytes.size()));
  (void)mine.u32();  // status
  (void)Bitmap::decode(mine);
  auto ours = mine.opaque(1u << 20);
  bool same = ours && std::equal(ours->begin(), ours->end(), vals->begin(), vals->end());
  if (nverify) code = same ? st(Status::kSame) : st(Status::kOk);
  else code = same ? st(Status::kOk) : st(Status::kNotSame);
  enc.u32(code);
  co_return code;
}

// ---- namespace ops ---------------------------------------------------------

rt::Task<uint32_t> Engine::op_create(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto type = dec.u32();
  if (!type) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  std::string linkdata;
  backend::DevT rdev{};
  if (*type == kNf4Lnk) {
    auto target = dec.string(kMaxSymlink);
    if (!target) {
      enc.u32(st(Status::kBadxdr));
      co_return st(Status::kBadxdr);
    }
    linkdata = *target;
  } else if (*type == kNf4Blk || *type == kNf4Chr) {
    auto major = dec.u32();
    auto minor = dec.u32();
    if (!major || !minor) {
      enc.u32(st(Status::kBadxdr));
      co_return st(Status::kBadxdr);
    }
    rdev = {*major, *minor};
  }
  auto name = dec.string(kMaxName + 1);
  if (!name) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  backend::SetAttr attrs;
  Bitmap attrset;
  Status decoded = decode_settable_fattr(dec, attrs, attrset);
  if (decoded != Status::kOk) {
    enc.u32(st(decoded));
    co_return st(decoded);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  bool known = *type == kNf4Dir || *type == kNf4Lnk || *type == kNf4Blk ||
               *type == kNf4Chr || *type == kNf4Sock || *type == kNf4Fifo;
  if (!known) {  // regular files are created by OPEN (RFC 8881 §18.4.3)
    enc.u32(st(Status::kBadtype));
    co_return st(Status::kBadtype);
  }
  if (*type == kNf4Lnk && linkdata.empty()) {
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  auto dir = co_await resolve(ctx, ctx.cfh);
  if (!dir) {
    uint32_t code = st(core::to_v4(dir.error(), Op::kCreate));
    enc.u32(code);
    co_return code;
  }
  if (dir->pseudo()) {
    enc.u32(st(Status::kRofs));
    co_return st(Status::kRofs);
  }
  if (dir->obj->type() != backend::FType::kDir) {
    enc.u32(st(Status::kNotdir));
    co_return st(Status::kNotdir);
  }
  // Mutate-guard order (plan doc 10 §6.1): readonly -> name -> capability gates.
  MutateGuard guard(locks_, exports_, *dir->exp, ctx.cred);
  if (auto verdict = guard.precheck({*name}); !verdict) {
    uint32_t code = verdict_status4(verdict);
    enc.u32(code);
    co_return code;
  }
  if (!utf8_component(*name)) {
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  auto caps = dir->exp->backend->caps();
  if ((*type == kNf4Lnk && !caps.has(backend::Cap::kSymlink)) ||
      ((*type == kNf4Blk || *type == kNf4Chr || *type == kNf4Sock ||
        *type == kNf4Fifo) &&
       !caps.has(backend::Cap::kMknod))) {
    enc.u32(st(Status::kNotsupp));
    co_return st(Status::kNotsupp);
  }
  attrs.size.reset();  // size is meaningless for these object types
  co_await guard.enter({dir->obj, dir->oid});
  const backend::Cred& cred = guard.cred();
  Result<backend::Created> made = Err(errno_from(EIO));
  switch (*type) {
    case kNf4Dir: made = co_await dir->obj->mkdir(cred, *name, attrs); break;
    case kNf4Lnk: made = co_await dir->obj->symlink(cred, *name, linkdata, attrs); break;
    case kNf4Blk:
      made = co_await dir->obj->mknod(cred, *name, backend::FType::kBlk, rdev, attrs);
      break;
    case kNf4Chr:
      made = co_await dir->obj->mknod(cred, *name, backend::FType::kChr, rdev, attrs);
      break;
    case kNf4Sock:
      made = co_await dir->obj->mknod(cred, *name, backend::FType::kSock, {}, attrs);
      break;
    case kNf4Fifo:
      made = co_await dir->obj->mknod(cred, *name, backend::FType::kFifo, {}, attrs);
      break;
    default: break;
  }
  co_await guard.finish();
  if (!made) {
    uint32_t code = st(core::to_v4(made.error(), Op::kCreate));
    enc.u32(code);
    co_return code;
  }
  ctx.cfh = export_fh(*dir->exp, made->obj->id());
  enc.u32(st(Status::kOk));
  encode_change_info(enc, true, guard.first().change_before(),
                     guard.first().change_after());
  attrset.encode(enc);
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_remove(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto name = dec.string(kMaxName + 1);
  if (!name) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  auto dir = co_await resolve(ctx, ctx.cfh);
  if (!dir) {
    uint32_t code = st(core::to_v4(dir.error(), Op::kRemove));
    enc.u32(code);
    co_return code;
  }
  if (dir->pseudo()) {
    enc.u32(st(Status::kRofs));
    co_return st(Status::kRofs);
  }
  if (dir->obj->type() != backend::FType::kDir) {
    enc.u32(st(Status::kNotdir));
    co_return st(Status::kNotdir);
  }
  MutateGuard guard(locks_, exports_, *dir->exp, ctx.cred);
  if (auto verdict = guard.precheck({*name}); !verdict) {
    uint32_t code = verdict_status4(verdict);
    enc.u32(code);
    co_return code;
  }
  if (!utf8_component(*name)) {
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  co_await guard.enter({dir->obj, dir->oid});
  const backend::Cred& cred = guard.cred();
  auto target = co_await dir->obj->lookup(cred, *name);
  if (!target) {
    uint32_t code = st(core::to_v4(target.error(), Op::kRemove));
    enc.u32(code);
    co_return code;
  }
  // Deleting a delegated file recalls its read delegations first (plan doc 10 §5.2).
  if (uint32_t dc =
          co_await state_.deleg_conflict(dir->exp->fsid, (*target)->id());
      dc != 0) {
    enc.u32(dc);
    co_return dc;
  }
  Result<void> removed = (*target)->type() == backend::FType::kDir
                             ? co_await dir->obj->rmdir(cred, *name)
                             : co_await dir->obj->unlink(cred, *name);
  co_await guard.finish();
  if (!removed) {
    uint32_t code = st(core::to_v4(removed.error(), Op::kRemove));
    enc.u32(code);
    co_return code;
  }
  enc.u32(st(Status::kOk));
  encode_change_info(enc, true, guard.first().change_before(),
                     guard.first().change_after());
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_rename(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto oldname = dec.string(kMaxName + 1);
  auto newname = dec.string(kMaxName + 1);
  if (!oldname || !newname) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  if (ctx.sfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  auto from = co_await resolve(ctx, ctx.sfh);
  if (!from) {
    uint32_t code = st(core::to_v4(from.error(), Op::kRename));
    enc.u32(code);
    co_return code;
  }
  auto to = co_await resolve(ctx, ctx.cfh);
  if (!to) {
    uint32_t code = st(core::to_v4(to.error(), Op::kRename));
    enc.u32(code);
    co_return code;
  }
  if (from->pseudo() || to->pseudo()) {
    enc.u32(st(Status::kRofs));
    co_return st(Status::kRofs);
  }
  if (from->obj->type() != backend::FType::kDir || to->obj->type() != backend::FType::kDir) {
    enc.u32(st(Status::kNotdir));
    co_return st(Status::kNotdir);
  }
  if (from->exp != to->exp) {  // not expressible for the backend (design 04 §4.2)
    enc.u32(st(Status::kXdev));
    co_return st(Status::kXdev);
  }
  MutateGuard guard(locks_, exports_, *from->exp, ctx.cred);
  if (auto verdict = guard.precheck({*oldname, *newname}); !verdict) {
    uint32_t code = verdict_status4(verdict);
    enc.u32(code);
    co_return code;
  }
  if (!utf8_component(*oldname) || !utf8_component(*newname)) {
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  // Two-directory lock ordering by ObjId happens inside the guard (design 04 §4.2).
  co_await guard.enter({from->obj, from->oid}, {to->obj, to->oid});
  const backend::Cred& cred = guard.cred();
  // A rename that replaces an existing target deletes it: recall the target's read
  // delegations first (plan doc 10 §5.2).  The source only changes name; its oid —
  // and therefore its delegations — stay coherent.
  if (auto victim = co_await to->obj->lookup(cred, *newname); victim) {
    if (uint32_t dc =
            co_await state_.deleg_conflict(to->exp->fsid, (*victim)->id());
        dc != 0) {
      enc.u32(dc);
      co_return dc;
    }
  }
  auto renamed = co_await from->obj->rename(cred, *oldname, *to->obj, *newname);
  co_await guard.finish();
  if (!renamed) {
    uint32_t code = st(core::to_v4(renamed.error(), Op::kRename));
    enc.u32(code);
    co_return code;
  }
  enc.u32(st(Status::kOk));
  encode_change_info(enc, true, guard.first().change_before(),
                     guard.first().change_after());
  encode_change_info(enc, true, guard.second().change_before(),
                     guard.second().change_after());
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_link(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto newname = dec.string(kMaxName + 1);
  if (!newname) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty() || ctx.sfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  auto file = co_await resolve(ctx, ctx.sfh);
  if (!file) {
    uint32_t code = st(core::to_v4(file.error(), Op::kLink));
    enc.u32(code);
    co_return code;
  }
  auto dir = co_await resolve(ctx, ctx.cfh);
  if (!dir) {
    uint32_t code = st(core::to_v4(dir.error(), Op::kLink));
    enc.u32(code);
    co_return code;
  }
  if (file->pseudo() || dir->pseudo()) {
    enc.u32(st(Status::kRofs));
    co_return st(Status::kRofs);
  }
  if (file->obj->type() == backend::FType::kDir) {
    enc.u32(st(Status::kIsdir));
    co_return st(Status::kIsdir);
  }
  if (dir->obj->type() != backend::FType::kDir) {
    enc.u32(st(Status::kNotdir));
    co_return st(Status::kNotdir);
  }
  if (file->exp != dir->exp) {
    enc.u32(st(Status::kXdev));
    co_return st(Status::kXdev);
  }
  // Mutate-guard order (plan doc 10 §6.1): readonly -> name -> capability gate.
  MutateGuard guard(locks_, exports_, *dir->exp, ctx.cred);
  if (auto verdict = guard.precheck({*newname}); !verdict) {
    uint32_t code = verdict_status4(verdict);
    enc.u32(code);
    co_return code;
  }
  if (!utf8_component(*newname)) {
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  if (!dir->exp->backend->caps().has(backend::Cap::kHardlink)) {
    enc.u32(st(Status::kNotsupp));
    co_return st(Status::kNotsupp);
  }
  // The new link's directory carries the change_info; the file is locked unsampled.
  co_await guard.enter({dir->obj, dir->oid}, {file->obj, file->oid, /*sample=*/false});
  auto linked = co_await dir->obj->link(guard.cred(), *file->obj, *newname);
  co_await guard.finish();
  if (!linked) {
    uint32_t code = st(core::to_v4(linked.error(), Op::kLink));
    enc.u32(code);
    co_return code;
  }
  enc.u32(st(Status::kOk));
  encode_change_info(enc, true, guard.first().change_before(),
                     guard.first().change_after());
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
    auto resolved = co_await resolve(ctx, ctx.cfh);
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
  if (uint32_t cur = resolve_current(ctx, *sid); cur != st(Status::kOk)) {
    enc.u32(cur);
    co_return cur;
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

// ---- byte-range locks (phase 5: design 07 §7.6) ------------------------------

namespace {

constexpr uint32_t kReadLt = 1, kWriteLt = 2, kWriteWLt = 4;
[[maybe_unused]] constexpr uint32_t kReadWLt = 3;  // range-checked via kWriteWLt bound only

// Validates locktype/offset/length per RFC 8881 §18.10.3; returns 0 or a status.
uint32_t check_lock_range(uint32_t locktype, uint64_t offset, uint64_t length) {
  if (locktype < kReadLt || locktype > kWriteWLt) return st(Status::kInval);
  if (length == 0) return st(Status::kInval);
  if (length != UINT64_MAX && offset > UINT64_MAX - length) return st(Status::kInval);
  return 0;
}

void encode_lock_denied(xdr::XdrEnc& enc, const state::StateMgr::LockDenied& d) {
  enc.u64(d.offset);
  enc.u64(d.length);
  enc.u32(d.exclusive ? kWriteLt : kReadLt);
  enc.u64(d.clientid);
  enc.opaque(std::span<const std::byte>(reinterpret_cast<const std::byte*>(d.owner.data()),
                                        d.owner.size()));
}

}  // namespace

// Resolves the CFH to a regular file on an export for the lock ops.
rt::Task<Result<Engine::Resolved>> Engine::resolve_lock_target(Ctx& ctx, uint32_t* status) {
  *status = 0;
  if (ctx.cfh.empty()) {
    *status = st(Status::kNofilehandle);
    co_return Err(errno_from(EINVAL));
  }
  auto resolved = co_await resolve(ctx, ctx.cfh);
  if (!resolved) {
    *status = st(core::to_v4(resolved.error(), Op::kLock));
    co_return Err(resolved.error());
  }
  if (resolved->pseudo() || resolved->obj->type() == backend::FType::kDir) {
    *status = st(Status::kIsdir);
    co_return Err(errno_from(EISDIR));
  }
  if (resolved->obj->type() != backend::FType::kReg) {
    *status = st(Status::kWrongType);
    co_return Err(errno_from(EINVAL));
  }
  co_return resolved;
}

rt::Task<uint32_t> Engine::op_lock(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto locktype = dec.u32();
  auto reclaim = dec.boolean();
  auto offset = dec.u64();
  auto length = dec.u64();
  auto new_owner = dec.boolean();
  if (!locktype || !reclaim || !offset || !length || !new_owner) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  state::StateMgr::LockArgs args;
  args.new_owner = *new_owner;
  if (*new_owner) {  // open_to_lock_owner4
    auto open_seqid = dec.u32();  // 4.0 owner seqids: ignored in 4.1
    auto open_sid = Stateid::decode(dec);
    auto lock_seqid = dec.u32();
    auto owner_client = dec.u64();
    auto owner = dec.opaque(kMaxOwnerId);
    if (!open_seqid || !open_sid || !lock_seqid || !owner_client || !owner) {
      enc.u32(st(Status::kBadxdr));
      co_return st(Status::kBadxdr);
    }
    args.open_stateid = *open_sid;
    args.owner.assign(reinterpret_cast<const char*>(owner->data()), owner->size());
  } else {  // exist_lock_owner4
    auto lock_sid = Stateid::decode(dec);
    auto lock_seqid = dec.u32();
    if (!lock_sid || !lock_seqid) {
      enc.u32(st(Status::kBadxdr));
      co_return st(Status::kBadxdr);
    }
    args.lock_stateid = *lock_sid;
  }
  if (uint32_t code = check_lock_range(*locktype, *offset, *length); code != 0) {
    enc.u32(code);
    co_return code;
  }
  uint32_t code = 0;
  auto target = co_await resolve_lock_target(ctx, &code);
  if (!target) {
    enc.u32(code);
    co_return code;
  }
  Stateid* anchor = args.new_owner ? &args.open_stateid : &args.lock_stateid;
  if (uint32_t cur = resolve_current(ctx, *anchor); cur != st(Status::kOk)) {
    enc.u32(cur);
    co_return cur;
  }
  args.clientid = ctx.clientid;
  args.fsid = target->exp->fsid;
  args.oid = target->oid;
  args.exclusive = *locktype == kWriteLt || *locktype == kWriteWLt;
  args.reclaim = *reclaim;
  args.offset = *offset;
  args.length = *length;
  const bool waiter_known = args.new_owner;  // first attempts carry the owner bytes
  std::string waiter_owner = args.owner;
  auto result = co_await state_.lock(std::move(args));
  enc.u32(result.status);
  if (result.status == st(Status::kDenied)) {
    // CB_NOTIFY_LOCK (plan doc 10 §5.2): remember the blocked owner; when a lock on
    // this file is released the client is poked to retry instead of blind-polling.
    if (ctx.session && waiter_known)
      state_.register_lock_waiter(target->exp->fsid, target->oid, ctx.clientid,
                                  std::move(waiter_owner), ctx.cfh);
    encode_lock_denied(enc, result.denied);
  } else if (result.status == 0) {
    ctx.current_sid = result.stateid;
    ctx.current_valid = true;
    result.stateid.encode(enc);
  }
  co_return result.status;
}

rt::Task<uint32_t> Engine::op_lockt(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto locktype = dec.u32();
  auto offset = dec.u64();
  auto length = dec.u64();
  auto owner_client = dec.u64();
  auto owner = dec.opaque(kMaxOwnerId);
  if (!locktype || !offset || !length || !owner_client || !owner) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (uint32_t code = check_lock_range(*locktype, *offset, *length); code != 0) {
    enc.u32(code);
    co_return code;
  }
  uint32_t code = 0;
  auto target = co_await resolve_lock_target(ctx, &code);
  if (!target) {
    enc.u32(code);
    co_return code;
  }
  auto result = co_await state_.lockt(
      ctx.clientid, target->exp->fsid, target->oid,
      std::string(reinterpret_cast<const char*>(owner->data()), owner->size()),
      *locktype == kWriteLt || *locktype == kWriteWLt, *offset, *length);
  enc.u32(result.status);
  if (result.status == st(Status::kDenied)) encode_lock_denied(enc, result.denied);
  co_return result.status;
}

rt::Task<uint32_t> Engine::op_locku(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto locktype = dec.u32();
  auto seqid = dec.u32();
  auto sid = Stateid::decode(dec);
  auto offset = dec.u64();
  auto length = dec.u64();
  if (!locktype || !seqid || !sid || !offset || !length) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (uint32_t code = check_lock_range(*locktype, *offset, *length); code != 0) {
    enc.u32(code);
    co_return code;
  }
  uint32_t code = 0;
  auto target = co_await resolve_lock_target(ctx, &code);
  if (!target) {
    enc.u32(code);
    co_return code;
  }
  if (uint32_t cur = resolve_current(ctx, *sid); cur != st(Status::kOk)) {
    enc.u32(cur);
    co_return cur;
  }
  Stateid out;
  code = co_await state_.locku(*sid, ctx.clientid, *offset, *length, &out);
  enc.u32(code);
  if (code == 0) {
    ctx.current_sid = out;
    ctx.current_valid = true;
    out.encode(enc);
  }
  co_return code;
}

// SECINFO (RFC 8881 §18.29): AUTH_SYS-only server — the answer is always [AUTH_SYS]
// once the name resolves; the current filehandle is consumed (§2.6.3.1.1.8).
rt::Task<uint32_t> Engine::op_secinfo(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto name = dec.string(kMaxName + 1);
  if (!name) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  if (ctx.cfh.empty()) {
    enc.u32(st(Status::kNofilehandle));
    co_return st(Status::kNofilehandle);
  }
  if (name->empty() || !utf8_component(*name)) {
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  if (!core::valid_component(*name)) {
    enc.u32(st(Status::kBadname));
    co_return st(Status::kBadname);
  }
  auto resolved = co_await resolve(ctx, ctx.cfh);
  if (!resolved) {
    uint32_t code = st(core::to_v4(resolved.error(), Op::kSecinfo));
    enc.u32(code);
    co_return code;
  }
  if (resolved->pseudo()) {
    if (!resolved->node->children.contains(*name)) {
      enc.u32(st(Status::kNoent));
      co_return st(Status::kNoent);
    }
  } else {
    if (resolved->obj->type() != backend::FType::kDir) {
      enc.u32(st(Status::kNotdir));
      co_return st(Status::kNotdir);
    }
    auto mapped = exports_.squash_cred(ctx.cred, *resolved->exp);
    auto cred = mapped.view();
    auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
    auto held = co_await lock->lock_shared();
    auto found = co_await resolved->obj->lookup(cred, *name);
    if (!found) {
      uint32_t code = st(core::to_v4(found.error(), Op::kSecinfo));
      enc.u32(code);
      co_return code;
    }
  }
  enc.u32(st(Status::kOk));
  enc.u32(1);  // one flavor
  enc.u32(1);  // AUTH_SYS
  ctx.cfh.clear();
  ctx.current_valid = false;
  co_return st(Status::kOk);
}

// ---- sessionless operations ------------------------------------------------

// ---- v4.2 sweets (RFC 7862 §15; development plan §8 item 1) ------------------------

rt::Task<Result<Engine::Resolved>> Engine::resolve_regular(Ctx& ctx, const FhBytes& fh,
                                                           Op op, uint32_t* status) {
  *status = 0;
  if (fh.empty()) {
    *status = st(Status::kNofilehandle);
    co_return Err(errno_from(EINVAL));
  }
  auto resolved = co_await resolve(ctx, fh);
  if (!resolved) {
    *status = st(core::to_v4(resolved.error(), op));
    co_return Err(resolved.error());
  }
  if (resolved->pseudo() || resolved->obj->type() == backend::FType::kDir) {
    *status = st(Status::kIsdir);
    co_return Err(errno_from(EISDIR));
  }
  if (resolved->obj->type() != backend::FType::kReg) {
    *status = st(Status::kWrongType);
    co_return Err(errno_from(EINVAL));
  }
  co_return resolved;
}

rt::Task<uint32_t> Engine::op_seek(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto sid = Stateid::decode(dec);
  auto offset = dec.u64();
  auto what = dec.u32();
  if (!sid || !offset || !what) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  uint32_t status = 0;
  auto resolved = co_await resolve_regular(ctx, ctx.cfh, Op::kSeek, &status);
  if (!resolved) {
    enc.u32(status);
    co_return status;
  }
  if (*what != kContentData && *what != kContentHole) {
    enc.u32(st(Status::kUnionNotsupp));
    co_return st(Status::kUnionNotsupp);
  }
  if (!resolved->exp->backend->caps().has(backend::Cap::kSparseOps)) {
    enc.u32(st(Status::kNotsupp));
    co_return st(Status::kNotsupp);
  }
  if (uint32_t cur = resolve_current(ctx, *sid); cur != st(Status::kOk)) {
    enc.u32(cur);
    co_return cur;
  }
  auto check = co_await state_.check_io(*sid, ctx.clientid, resolved->exp->fsid,
                                        resolved->oid, state::kShareRead);
  if (check.status != 0) {
    enc.u32(check.status);
    co_return check.status;
  }
  auto mapped = exports_.squash_cred(ctx.cred, *resolved->exp);
  auto cred = mapped.view();
  auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
  auto held = co_await lock->lock_shared();
  backend::OpenCtx open{cred, check.bopen.get()};
  auto found = co_await resolved->obj->seek(
      open, *offset, *what == kContentData ? backend::SeekWhat::kData : backend::SeekWhat::kHole);
  if (!found) {
    uint32_t code = st(core::to_v4(found.error(), Op::kSeek));
    enc.u32(code);
    co_return code;
  }
  // eof mirrors knfsd: the returned position is at/after the current size.
  auto attr = co_await resolved->obj->getattr();
  bool eof = attr ? *found >= attr->size : false;
  enc.u32(st(Status::kOk));
  enc.boolean(eof);
  enc.u64(*found);
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_allocate(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc,
                                       bool deallocate) {
  const Op op = deallocate ? Op::kDeallocate : Op::kAllocate;
  auto sid = Stateid::decode(dec);
  auto offset = dec.u64();
  auto length = dec.u64();
  if (!sid || !offset || !length) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  uint32_t status = 0;
  auto resolved = co_await resolve_regular(ctx, ctx.cfh, op, &status);
  if (!resolved) {
    enc.u32(status);
    co_return status;
  }
  if (!resolved->exp->backend->caps().has(backend::Cap::kSparseOps)) {
    enc.u32(st(Status::kNotsupp));
    co_return st(Status::kNotsupp);
  }
  if (*length == 0 || *offset + *length < *offset) {  // RFC 7862 §15.1.3 / §15.4.3
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  if (uint32_t cur = resolve_current(ctx, *sid); cur != st(Status::kOk)) {
    enc.u32(cur);
    co_return cur;
  }
  auto check = co_await state_.check_io(*sid, ctx.clientid, resolved->exp->fsid,
                                        resolved->oid, state::kShareWrite);
  if (check.status != 0) {
    enc.u32(check.status);
    co_return check.status;
  }
  MutateGuard guard(locks_, exports_, *resolved->exp, ctx.cred);
  if (auto verdict = guard.precheck({}); !verdict) {
    uint32_t code = verdict_status4(verdict);
    enc.u32(code);
    co_return code;
  }
  co_await guard.enter({resolved->obj, resolved->oid, /*sample=*/false});
  backend::OpenCtx open{guard.cred(), check.bopen.get()};
  auto done = deallocate ? co_await resolved->obj->deallocate(open, *offset, *length)
                         : co_await resolved->obj->allocate(open, *offset, *length);
  if (!done) {
    uint32_t code = st(core::to_v4(done.error(), op));
    enc.u32(code);
    co_return code;
  }
  enc.u32(st(Status::kOk));
  co_return st(Status::kOk);
}

// COPY/CLONE: src = SFH, dst = CFH, both regular files on the same export (cross-export
// -> XDEV: the backend boundary is the copy domain).
rt::Task<uint32_t> Engine::op_copy(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto src_sid = Stateid::decode(dec);
  auto dst_sid = Stateid::decode(dec);
  auto src_off = dec.u64();
  auto dst_off = dec.u64();
  auto count = dec.u64();
  auto consecutive = dec.boolean();
  auto synchronous = dec.boolean();
  auto nservers = dec.u32();
  if (!src_sid || !dst_sid || !src_off || !dst_off || !count || !consecutive ||
      !synchronous || !nservers) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  uint32_t status = 0;
  auto dst = co_await resolve_regular(ctx, ctx.cfh, Op::kCopy, &status);
  if (!dst) {
    enc.u32(status);
    co_return status;
  }
  auto src = co_await resolve_regular(ctx, ctx.sfh, Op::kCopy, &status);
  if (!src) {
    enc.u32(status);
    co_return status;
  }
  if (*nservers != 0) {  // inter-server copy: not offered (research 08 §8.1)
    enc.u32(st(Status::kNotsupp));
    co_return st(Status::kNotsupp);
  }
  if (!dst->exp->backend->caps().has(backend::Cap::kCopyRange)) {
    enc.u32(st(Status::kNotsupp));
    co_return st(Status::kNotsupp);
  }
  if (src->exp != dst->exp) {
    enc.u32(st(Status::kXdev));
    co_return st(Status::kXdev);
  }
  if (*src_off + *count < *src_off || *dst_off + *count < *dst_off) {
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  // Stateids: the destination follows the current-stateid rule; a placeholder source
  // stateid refers to the one saved with SAVEFH (falls back to current).
  if (is_current_placeholder(*src_sid)) {
    if (ctx.saved_valid) *src_sid = ctx.saved_sid;
    else if (uint32_t cur = resolve_current(ctx, *src_sid); cur != st(Status::kOk)) {
      enc.u32(cur);
      co_return cur;
    }
  }
  if (uint32_t cur = resolve_current(ctx, *dst_sid); cur != st(Status::kOk)) {
    enc.u32(cur);
    co_return cur;
  }
  auto scheck = co_await state_.check_io(*src_sid, ctx.clientid, src->exp->fsid, src->oid,
                                         state::kShareRead);
  if (scheck.status != 0) {
    enc.u32(scheck.status);
    co_return scheck.status;
  }
  auto dcheck = co_await state_.check_io(*dst_sid, ctx.clientid, dst->exp->fsid, dst->oid,
                                         state::kShareWrite);
  if (dcheck.status != 0) {
    enc.u32(dcheck.status);
    co_return dcheck.status;
  }
  if (dst->exp->readonly) {
    enc.u32(st(Status::kRofs));
    co_return st(Status::kRofs);
  }
  auto mapped = exports_.squash_cred(ctx.cred, *dst->exp);
  auto cred = mapped.view();
  backend::OpenCtx sopen{cred, scheck.bopen.get()};
  backend::OpenCtx dopen{cred, dcheck.bopen.get()};
  // Chunked copy with the locks released between chunks (plan doc 10 §5.2): the old
  // whole-range hold blocked every request on both files for the entire copy —
  // unbounded with ca_count == 0.  Each chunk is copied under the deterministic
  // two-object lock, then the locks drop so readers/writers interleave; the total is
  // not atomic, which POSIX-mapped COPY never promised (the reply stays UNSTABLE +
  // verifier, like WRITE).
  constexpr uint64_t kCopyChunk = 8u << 20;
  bool same = src->oid == dst->oid;
  auto first = locks_.get(dst->exp->fsid, same || src->oid < dst->oid ? src->oid : dst->oid);
  auto second = same ? nullptr
                     : locks_.get(dst->exp->fsid, src->oid < dst->oid ? dst->oid : src->oid);
  uint64_t length = *count;
  {
    auto held1 = co_await first->lock();
    std::optional<decltype(held1)> held2;
    if (second) held2.emplace(co_await second->lock());
    if (length == 0) {  // ca_count 0: through the source EOF (RFC 7862 §15.2.3)
      auto attr = co_await src->obj->getattr();
      if (!attr) {
        uint32_t code = st(core::to_v4(attr.error(), Op::kCopy));
        enc.u32(code);
        co_return code;
      }
      length = attr->size > *src_off ? attr->size - *src_off : 0;
    }
  }
  Result<uint64_t> copied = 0;
  uint64_t done = 0;
  while (done < length) {
    uint64_t chunk = std::min(kCopyChunk, length - done);
    auto held1 = co_await first->lock();
    std::optional<decltype(held1)> held2;
    if (second) held2.emplace(co_await second->lock());
    auto part = co_await src->obj->copy_range(sopen, *dst->obj, dopen, *src_off + done,
                                              *dst_off + done, chunk);
    if (!part) {
      copied = Err(part.error());
      break;
    }
    done += *part;
    copied = done;
    if (*part < chunk) break;  // source EOF inside the chunk
  }
  if (!copied) {
    uint32_t code = st(core::to_v4(copied.error(), Op::kCopy));
    enc.u32(code);
    co_return code;
  }
  obs::Metrics::instance().write_bytes.fetch_add(*copied, std::memory_order_relaxed);
  dst->exp->metrics.write_bytes.fetch_add(*copied, std::memory_order_relaxed);
  dst->exp->metrics.write_ops.fetch_add(1, std::memory_order_relaxed);
  enc.u32(st(Status::kOk));
  // write_response4: no callback stateid (synchronous), count, UNSTABLE + verifier —
  // the client COMMITs like after WRITE (RFC 7862 §15.2.3).
  enc.u32(0);
  enc.u64(*copied);
  enc.u32(0);
  enc.opaque_fixed(write_verf_);
  enc.boolean(true);  // cr_consecutive
  enc.boolean(true);  // cr_synchronous (asynchronous requests are served synchronously)
  co_return st(Status::kOk);
}

rt::Task<uint32_t> Engine::op_clone(Ctx& ctx, xdr::XdrDec& dec, xdr::XdrEnc& enc) {
  auto src_sid = Stateid::decode(dec);
  auto dst_sid = Stateid::decode(dec);
  auto src_off = dec.u64();
  auto dst_off = dec.u64();
  auto count = dec.u64();
  if (!src_sid || !dst_sid || !src_off || !dst_off || !count) {
    enc.u32(st(Status::kBadxdr));
    co_return st(Status::kBadxdr);
  }
  uint32_t status = 0;
  auto dst = co_await resolve_regular(ctx, ctx.cfh, Op::kClone, &status);
  if (!dst) {
    enc.u32(status);
    co_return status;
  }
  auto src = co_await resolve_regular(ctx, ctx.sfh, Op::kClone, &status);
  if (!src) {
    enc.u32(status);
    co_return status;
  }
  if (!dst->exp->backend->caps().has(backend::Cap::kCloneRange)) {
    enc.u32(st(Status::kNotsupp));
    co_return st(Status::kNotsupp);
  }
  if (src->exp != dst->exp) {
    enc.u32(st(Status::kXdev));
    co_return st(Status::kXdev);
  }
  if (*src_off + *count < *src_off || *dst_off + *count < *dst_off) {
    enc.u32(st(Status::kInval));
    co_return st(Status::kInval);
  }
  if (is_current_placeholder(*src_sid)) {
    if (ctx.saved_valid) *src_sid = ctx.saved_sid;
    else if (uint32_t cur = resolve_current(ctx, *src_sid); cur != st(Status::kOk)) {
      enc.u32(cur);
      co_return cur;
    }
  }
  if (uint32_t cur = resolve_current(ctx, *dst_sid); cur != st(Status::kOk)) {
    enc.u32(cur);
    co_return cur;
  }
  auto scheck = co_await state_.check_io(*src_sid, ctx.clientid, src->exp->fsid, src->oid,
                                         state::kShareRead);
  if (scheck.status != 0) {
    enc.u32(scheck.status);
    co_return scheck.status;
  }
  auto dcheck = co_await state_.check_io(*dst_sid, ctx.clientid, dst->exp->fsid, dst->oid,
                                         state::kShareWrite);
  if (dcheck.status != 0) {
    enc.u32(dcheck.status);
    co_return dcheck.status;
  }
  MutateGuard guard(locks_, exports_, *dst->exp, ctx.cred);
  if (auto verdict = guard.precheck({}); !verdict) {
    uint32_t code = verdict_status4(verdict);
    enc.u32(code);
    co_return code;
  }
  // Same ObjId-ordered two-object locking as RENAME/LINK, via the guard.
  co_await guard.enter({src->obj, src->oid, /*sample=*/false},
                       {dst->obj, dst->oid, /*sample=*/false});
  backend::OpenCtx sopen{guard.cred(), scheck.bopen.get()};
  backend::OpenCtx dopen{guard.cred(), dcheck.bopen.get()};
  uint64_t length = *count;
  if (length == 0) {  // cl_count 0: through the source EOF (RFC 7862 §15.13.3)
    auto attr = co_await src->obj->getattr();
    if (!attr) {
      uint32_t code = st(core::to_v4(attr.error(), Op::kClone));
      enc.u32(code);
      co_return code;
    }
    if (attr->size <= *src_off) {
      enc.u32(st(Status::kInval));
      co_return st(Status::kInval);
    }
    length = attr->size - *src_off;
  }
  auto cloned = co_await src->obj->clone(sopen, *dst->obj, dopen, *src_off, *dst_off, length);
  if (!cloned) {
    uint32_t code = st(core::to_v4(cloned.error(), Op::kClone));
    enc.u32(code);
    co_return code;
  }
  enc.u32(st(Status::kOk));
  co_return st(Status::kOk);
}

// ---- sessionless ops --------------------------------------------------------

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
  enc.string(server_owner_);  // server_owner.major_id (stable across restarts)
  enc.string(server_scope_);  // server_scope
  enc.u32(0);                 // server_impl_id: empty
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
  cb::Cred cb_cred;  // first AUTH_SYS wins; else callbacks go out AUTH_NONE (§2.10.8.2)
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
      if (!cb_cred.auth_sys) {
        cb_cred.auth_sys = true;
        cb_cred.uid = *uid;
        cb_cred.gid = *gid;
        cb_cred.machine = std::string(*machine);
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
  // CONN_BACK_CHAN (0x2): this connection carries the backchannel (plan doc 10 §5.2).
  const bool want_back = (*flags & 0x2u) != 0;
  auto result = co_await state_.create_session(
      *clientid, *sequence, ctx.cred.principal(), *fore, *back, conn_id_of(ctx.conn),
      want_back ? ctx.conn.cb_channel() : nullptr, *cb_program, std::move(cb_cred));
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
  body.u32(want_back ? 0x2u : 0u);  // echo CONN_BACK_CHAN when the channel is bound
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
  // Backchannel directions (plan doc 10 §5.2): CDFC4_BACK(0x2)/FORE_OR_BOTH(0x3)/
  // BACK_OR_BOTH(0x7) attach this connection's channel to the session.
  bool back = (*dir & 0x2u) != 0 || *dir == 0x7u;
  if (back) {
    uint32_t bound = co_await state_.bind_backchannel(id, ctx.conn.cb_channel());
    if (bound != 0) {
      enc.u32(bound);
      co_return bound;
    }
  }
  enc.u32(st(Status::kOk));
  enc.opaque_fixed(id);
  enc.u32(back ? (*dir == 0x2u ? 0x2u : 0x3u) : 0x1u);  // CDFS4_BACK / BOTH / FORE
  enc.boolean(false);
  co_return st(Status::kOk);
}

}  // namespace lnfs::nfsv4
