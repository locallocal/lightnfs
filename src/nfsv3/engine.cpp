#include "nfsv3/engine.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>

#include "core/errmap.hpp"
#include "core/fs_props.hpp"
#include "core/mutate.hpp"
#include "core/names.hpp"
#include "core/readdir.hpp"
#include "obs/errlog.hpp"
#include "obs/metrics.hpp"
#include "rpc/drc.hpp"
#include "transport/connection.hpp"
#include "util/log.hpp"

namespace lnfs::nfsv3 {
namespace {

using rpc::RpcCall;
using transport::ConnCtx;
using core::MutateGuard;

// Send + optionally capture the reply bytes for the DRC (design 03 §3.7).  `cap` is
// null for procedures the DRC does not cache.
rt::Task<void> reply(ConnCtx& ctx, xdr::XdrEnc& enc, std::vector<std::byte>* cap) {
  auto buf = enc.take();
  if (cap) *cap = buf.to_bytes();
  co_await ctx.send(std::move(buf));
}

void begin_result(xdr::XdrEnc& enc, ConnCtx& ctx, const RpcCall& call, Status status) {
  rpc::encode_reply_success(enc, call.xid);
  enc.u32(static_cast<uint32_t>(status));
  if (status != Status::kOk) {
    obs::Metrics::instance().v3_errors[call.proc % obs::Metrics::kV3Procs].fetch_add(
        1, std::memory_order_relaxed);
    obs::record_error_reply(ctx.peer.to_string(), obs::v3_proc_name(call.proc), call.xid,
                            static_cast<uint32_t>(status));
  }
  if (log_enabled(LogLevel::kDebug))
    LNFS_DEBUG("v3 {} xid={:#x} peer={} st={}", obs::v3_proc_name(call.proc), call.xid,
               ctx.peer.to_string(), static_cast<uint32_t>(status));
}

// Aggregate per-procedure latency; destructor fires when the handler frame completes,
// exception paths included.  Requests over the slow threshold (plan doc 10 §3.6) leave
// a warn line — v3 procedures are single ops, so there is no further breakdown.
struct ProcTimer {
  ProcTimer(uint32_t p, const ConnCtx& c, uint32_t x)
      : proc(p % obs::Metrics::kV3Procs), ctx(&c), xid(x) {}
  ~ProcTimer() {
    auto us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - t0)
                                        .count());
    auto& m = obs::Metrics::instance();
    m.v3_calls[proc].fetch_add(1, std::memory_order_relaxed);
    m.v3_duration_us[proc].fetch_add(us, std::memory_order_relaxed);
    m.v3_duration[proc].observe_us(us);
    if (auto thr = obs::slow_request_threshold_us(); thr != 0 && us >= thr)
      LNFS_WARN("v3 slow request proc={} xid={:#x} peer={} dur={}us",
                obs::v3_proc_name(proc), xid, ctx->peer.to_string(), us);
  }
  uint32_t proc;
  const ConnCtx* ctx;
  uint32_t xid;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
};

// The nine non-idempotent procedures cached by the DRC (design 03 §3.7).
bool drc_cached(Proc proc) {
  switch (proc) {
    case Proc::kSetattr:
    case Proc::kCreate:
    case Proc::kMkdir:
    case Proc::kSymlink:
    case Proc::kMknod:
    case Proc::kRemove:
    case Proc::kRmdir:
    case Proc::kRename:
    case Proc::kLink: return true;
    default: return false;
  }
}

size_t aligned4(size_t value) { return (value + 3) & ~size_t(3); }
size_t dir_entry_size(std::string_view name) { return 24 + aligned4(name.size()); }

std::optional<backend::Attr> attr_value(const Result<backend::Attr>& result) {
  return result ? std::optional<backend::Attr>(*result) : std::nullopt;
}

backend::Stability stability_from_wire(uint32_t stable) {
  switch (stable) {
    case kUnstable: return backend::Stability::kUnstable;
    case kDataSync: return backend::Stability::kDataSync;
    default: return backend::Stability::kFileSync;
  }
}

// ---- WCC encoding from core::ChangeSample --------------------------------------
// wcc_data = pre-op sample (size/mtime/ctime) + post-op attributes; both come from the
// guard's samples.  A precheck failure (ROFS / bad name / unsupported) changed nothing,
// so its reply carries one sample as both before and after.

void encode_wcc_sample(xdr::XdrEnc& enc, const core::ChangeSample& sample,
                       uint64_t fsid) {
  encode_wcc(enc, wcc_pre(sample.before), sample.after, fsid);
}

void encode_wcc_unchanged(xdr::XdrEnc& enc, const std::optional<backend::Attr>& attr,
                          uint64_t fsid) {
  encode_wcc(enc, wcc_pre(attr), attr, fsid);
}

void encode_wcc_none(xdr::XdrEnc& enc) { encode_wcc(enc, std::nullopt, std::nullopt, 0); }

// v3 has one answer for every name-discipline failure on a creation-family call:
// ACCES.  RMDIR distinguishes "." (INVAL) and ".." (EXIST) per RFC 1813 §3.3.13.
Status verdict_status(const MutateGuard::Verdict& verdict) {
  switch (verdict.kind) {
    case MutateGuard::Verdict::kReadonly: return Status::kRofs;
    case MutateGuard::Verdict::kBadName: return Status::kAcces;
    default: return Status::kOk;
  }
}

}  // namespace

void Engine::register_with(rpc::Dispatcher& dispatcher) {
  dispatcher.add({kProgram, kVersion, kVersion, this,
                  [](void* self, ConnCtx& ctx, RpcCall& call, const rpc::Cred& cred) {
                    return static_cast<Engine*>(self)->dispatch(ctx, call, cred);
                  }});
}

rt::Task<Result<Engine::Resolved>> Engine::resolve(const FileHandle& fh,
                                                    const sockaddr_storage& peer) {
  auto decoded = handles_.decode(fh.data, peer);
  if (!decoded) co_return Err(decoded.error());
  auto obj = co_await decoded->export_entry->backend->resolve(decoded->oid);
  if (!obj) co_return Err(obj.error());
  co_return Resolved{decoded->export_entry, std::move(*obj), decoded->oid};
}

rt::Task<void> Engine::dispatch(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred) {
  ProcTimer timer(call.proc, ctx, call.xid);
  if (call.proc > static_cast<uint32_t>(Proc::kCommit)) {
    xdr::XdrEnc enc(ctx.pool);
    rpc::encode_reply_accepted_err(enc, call.xid, rpc::kProcUnavail);
    co_await reply(ctx, enc, nullptr);
    co_return;
  }
  Proc proc = static_cast<Proc>(call.proc);
  if (proc == Proc::kNull) {
    xdr::XdrEnc enc(ctx.pool);
    rpc::encode_reply_success(enc, call.xid);
    co_await reply(ctx, enc, nullptr);
    co_return;
  }

  if (drc_ && drc_cached(proc)) {
    auto key = rpc::Drc::Key::make(ctx.peer.addr, call.xid, call.prog, call.vers,
                                   call.proc, call.args_hash);
    auto claim = co_await drc_->begin(key);
    if (!claim.owner) {  // retransmission: replay the cached reply bytes verbatim
      xdr::XdrEnc enc(ctx.pool);
      enc.opaque_fixed(*claim.cached);
      co_await ctx.send(enc.take());
      co_return;
    }
    Capture cap;
    std::exception_ptr error;
    try {
      co_await dispatch_proc(ctx, call, rpc_cred, &cap);
    } catch (...) {
      error = std::current_exception();
    }
    // A reply was produced -> cache it; otherwise (exception, GARBAGE_ARGS path) drop
    // the claim so a waiting retransmission re-executes instead of hanging.  (JUKEBOX
    // never meets the cache: the 08 §8.2 whitelist admits it on READ/WRITE only, and
    // those idempotent procedures are not in drc_cached().)
    if (!error && !cap.empty()) co_await drc_->complete(key, std::move(cap));
    else co_await drc_->abort(key);
    if (error) std::rethrow_exception(error);
    co_return;
  }
  co_await dispatch_proc(ctx, call, rpc_cred, nullptr);
}

rt::Task<void> Engine::dispatch_proc(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                     Capture* cap) {
  // Indexed by Proc; NULL and out-of-range procedures never reach here (dispatch).
  static constexpr Handler kHandlers[] = {
      nullptr,                 // 0  NULL
      &Engine::proc_getattr,   // 1
      &Engine::proc_setattr,   // 2
      &Engine::proc_lookup,    // 3
      &Engine::proc_access,    // 4
      &Engine::proc_readlink,  // 5
      &Engine::proc_read,      // 6
      &Engine::proc_write,     // 7
      &Engine::proc_create,    // 8
      &Engine::proc_mkdir,     // 9
      &Engine::proc_symlink,   // 10
      &Engine::proc_mknod,     // 11
      &Engine::proc_remove,    // 12
      &Engine::proc_rmdir,     // 13
      &Engine::proc_rename,    // 14
      &Engine::proc_link,      // 15
      &Engine::proc_readdir,   // 16 READDIR
      &Engine::proc_readdir,   // 17 READDIRPLUS
      &Engine::proc_fs_query,  // 18 FSSTAT
      &Engine::proc_fs_query,  // 19 FSINFO
      &Engine::proc_fs_query,  // 20 PATHCONF
      &Engine::proc_commit,    // 21
  };
  static_assert(std::size(kHandlers) == static_cast<size_t>(Proc::kCommit) + 1);
  return (this->*kHandlers[call.proc])(ctx, call, rpc_cred, cap);
}

// ---- read-only procedures ----------------------------------------------------------

rt::Task<void> Engine::proc_getattr(ConnCtx& ctx, RpcCall& call, const rpc::Cred&,
                                    Capture* cap) {
  auto args = FileHandle::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto resolved = co_await resolve(*args, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!resolved) {
    begin_result(enc, ctx, call, core::to_v3(resolved.error(), Proc::kGetattr));
  } else {
    auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
    auto held = co_await lock->lock_shared();
    auto attr = co_await resolved->obj->getattr();
    if (!attr) begin_result(enc, ctx, call, core::to_v3(attr.error(), Proc::kGetattr));
    else {
      begin_result(enc, ctx, call, Status::kOk);
      encode_fattr(enc, *attr, resolved->exp->fsid);
    }
  }
  co_await reply(ctx, enc, cap);
}

rt::Task<void> Engine::proc_lookup(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                   Capture* cap) {
  auto args = Diropargs::decode(call.args);
  // LOOKUP may name "." / ".."; the creation family may not (core/names.hpp).
  if (!args || !call.args.at_end() || !core::valid_component(args->name, true)) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto dir = co_await resolve(args->dir, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!dir) {
    begin_result(enc, ctx, call, core::to_v3(dir.error(), Proc::kLookup));
    encode_post_attr(enc, std::nullopt, 0);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  auto mapped = exports_.squash_cred(rpc_cred, *dir->exp);
  auto cred = mapped.view();
  auto lock = locks_.get(dir->exp->fsid, dir->oid);
  auto held = co_await lock->lock_shared();
  auto found = co_await dir->obj->lookup(cred, args->name);
  auto dir_attr = co_await dir->obj->getattr();
  if (!found) {
    begin_result(enc, ctx, call, core::to_v3(found.error(), Proc::kLookup));
    encode_post_attr(enc, attr_value(dir_attr), dir->exp->fsid);
  } else {
    auto obj_attr = co_await (*found)->getattr();
    FileHandle wire{handles_.encode(*dir->exp, (*found)->id())};
    begin_result(enc, ctx, call, Status::kOk);
    wire.encode(enc);
    encode_post_attr(enc, attr_value(obj_attr), dir->exp->fsid);
    encode_post_attr(enc, attr_value(dir_attr), dir->exp->fsid);
  }
  co_await reply(ctx, enc, cap);
}

rt::Task<void> Engine::proc_access(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                   Capture* cap) {
  auto args = AccessArgs::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto resolved = co_await resolve(args->object, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!resolved) {
    begin_result(enc, ctx, call, core::to_v3(resolved.error(), Proc::kAccess));
    encode_post_attr(enc, std::nullopt, 0);
  } else {
    auto mapped = exports_.squash_cred(rpc_cred, *resolved->exp);
    auto cred = mapped.view();
    auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
    auto held = co_await lock->lock_shared();
    auto allowed = co_await resolved->obj->access(cred, access_from_wire(args->access));
    auto attr = co_await resolved->obj->getattr();
    if (!allowed) {
      begin_result(enc, ctx, call, core::to_v3(allowed.error(), Proc::kAccess));
      encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
    } else {
      if (resolved->exp->readonly)
        allowed->clear(backend::Access::kModify)
            .clear(backend::Access::kExtend)
            .clear(backend::Access::kDelete);
      begin_result(enc, ctx, call, Status::kOk);
      encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
      enc.u32(access_to_wire(*allowed));
    }
  }
  co_await reply(ctx, enc, cap);
}

rt::Task<void> Engine::proc_readlink(ConnCtx& ctx, RpcCall& call, const rpc::Cred&,
                                     Capture* cap) {
  auto args = FileHandle::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto resolved = co_await resolve(*args, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!resolved) {
    begin_result(enc, ctx, call, core::to_v3(resolved.error(), Proc::kReadlink));
    encode_post_attr(enc, std::nullopt, 0);
  } else {
    auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
    auto held = co_await lock->lock_shared();
    auto target = co_await resolved->obj->readlink();
    auto attr = co_await resolved->obj->getattr();
    if (!target) {
      begin_result(enc, ctx, call, core::to_v3(target.error(), Proc::kReadlink));
      encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
    } else {
      begin_result(enc, ctx, call, Status::kOk);
      encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
      enc.string(*target);
    }
  }
  co_await reply(ctx, enc, cap);
}

rt::Task<void> Engine::proc_read(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                 Capture* cap) {
  auto args = ReadArgs::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto resolved = co_await resolve(args->file, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!resolved) {
    begin_result(enc, ctx, call, core::to_v3(resolved.error(), Proc::kRead));
    encode_post_attr(enc, std::nullopt, 0);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  auto mapped = exports_.squash_cred(rpc_cred, *resolved->exp);
  auto cred = mapped.view();
  uint32_t count = std::min(args->count, resolved->exp->backend->limits().max_read);
  // Per-export QoS (plan doc 10 §4.3), before the shared object lock.
  co_await resolved->exp->qos.throttle(false, count);
  auto data = ctx.pool.alloc(std::max<uint32_t>(count, 1));
  bool eof = false;
  auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
  auto held = co_await lock->lock_shared();
  backend::OpenCtx open{cred, nullptr};
  auto read = co_await resolved->obj->read(open, args->offset,
                                           std::span<std::byte>(data.data(), count), eof);
  auto attr = co_await resolved->obj->getattr();
  if (!read) {
    begin_result(enc, ctx, call, core::to_v3(read.error(), Proc::kRead));
    encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
  } else {
    begin_result(enc, ctx, call, Status::kOk);
    encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
    obs::Metrics::instance().read_bytes.fetch_add(*read, std::memory_order_relaxed);
    resolved->exp->metrics.read_bytes.fetch_add(*read, std::memory_order_relaxed);
    resolved->exp->metrics.read_ops.fetch_add(1, std::memory_order_relaxed);
    enc.u32(*read);
    enc.boolean(eof);
    enc.u32(*read);
    enc.attach(std::move(data), 0, *read);
  }
  co_await reply(ctx, enc, cap);
}

// READDIR and READDIRPLUS share one body; `plus` selects the per-entry fh/attr tail.
rt::Task<void> Engine::proc_readdir(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                    Capture* cap) {
  Proc proc = static_cast<Proc>(call.proc);
  bool plus = proc == Proc::kReaddirplus;
  FileHandle dir_fh;
  uint64_t cookie = 0;
  uint32_t dircount = 0, maxcount = 0;
  std::array<std::byte, 8> client_verf{};
  if (plus) {
    auto args = ReaddirPlusArgs::decode(call.args);
    if (!args || !call.args.at_end()) {
      co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
      co_return;
    }
    dir_fh = std::move(args->dir);
    cookie = args->cookie;
    client_verf = args->cookieverf;
    dircount = args->dircount;
    maxcount = args->maxcount;
  } else {
    auto args = ReaddirArgs::decode(call.args);
    if (!args || !call.args.at_end()) {
      co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
      co_return;
    }
    dir_fh = std::move(args->dir);
    cookie = args->cookie;
    client_verf = args->cookieverf;
    dircount = maxcount = args->count;
  }
  auto resolved = co_await resolve(dir_fh, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!resolved) {
    begin_result(enc, ctx, call, core::to_v3(resolved.error(), proc));
    encode_post_attr(enc, std::nullopt, 0);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  auto mapped = exports_.squash_cred(rpc_cred, *resolved->exp);
  auto cred = mapped.view();
  auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
  auto held = co_await lock->lock_shared();
  auto attr = co_await resolved->obj->getattr();
  // Semantic cookie verifier (plan doc 10 §5.1): the directory change attribute.
  // A directory modified between pages makes the verifier mismatch; BAD_COOKIE
  // sends the client back to cookie 0 for a consistent restart instead of a
  // silently duplicated/holey listing.
  std::array<std::byte, 8> dir_verf{};
  uint64_t dir_change = attr ? attr->change : 0;
  std::memcpy(dir_verf.data(), &dir_change, sizeof(dir_change));
  if (cookie != 0 && client_verf != dir_verf) {
    begin_result(enc, ctx, call, Status::kBadCookie);
    encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  constexpr size_t kFixedReply = 24 + 4 + 88 + 8 + 8;
  if (maxcount < kFixedReply || dircount < 24) {
    begin_result(enc, ctx, call, Status::kToosmall);
    encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  uint32_t max_entries = std::min<uint32_t>(4096, std::max<uint32_t>(1, dircount / 24));
  auto page = co_await core::readdir_page(resolved->obj, cred, cookie, max_entries);
  if (!page) {
    Status status = cookie != 0 && page.error() == errno_from(EINVAL)
                        ? Status::kBadCookie
                        : core::to_v3(page.error(), proc);
    begin_result(enc, ctx, call, status);
    encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  begin_result(enc, ctx, call, Status::kOk);
  encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
  enc.opaque_fixed(dir_verf);
  size_t used_total = kFixedReply;
  size_t used_dir = 0;
  bool truncated = false;
  for (const auto& ent : page->ents) {
    size_t skeleton = dir_entry_size(ent.name);
    std::optional<FileHandle> item_fh;
    if (plus && ent.oid) item_fh = FileHandle{handles_.encode(*resolved->exp, *ent.oid)};
    size_t extra = 0;
    if (plus) {
      extra += ent.attr ? 88 : 4;
      extra += item_fh ? 8 + aligned4(item_fh->data.size()) : 4;
    }
    if (used_dir + skeleton > dircount || used_total + skeleton + extra > maxcount) {
      truncated = true;
      break;
    }
    enc.boolean(true);
    enc.u64(ent.fileid);
    enc.string(ent.name);
    enc.u64(ent.cookie);
    if (plus) {
      encode_post_attr(enc, ent.attr, resolved->exp->fsid);
      encode_post_fh(enc, item_fh);
    }
    used_dir += skeleton;
    used_total += skeleton + extra;
  }
  enc.boolean(false);
  enc.boolean(page->eof && !truncated);
  co_await reply(ctx, enc, cap);
}

// FSSTAT / FSINFO / PATHCONF all take one file handle and return post-op attributes;
// FSINFO/PATHCONF read the shared core::FsProps derivation (plan doc 10 §6.4).
rt::Task<void> Engine::proc_fs_query(ConnCtx& ctx, RpcCall& call, const rpc::Cred&,
                                     Capture* cap) {
  Proc proc = static_cast<Proc>(call.proc);
  auto args = FileHandle::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto resolved = co_await resolve(*args, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!resolved) {
    begin_result(enc, ctx, call, core::to_v3(resolved.error(), proc));
    encode_post_attr(enc, std::nullopt, 0);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
  auto held = co_await lock->lock_shared();
  auto attr = co_await resolved->obj->getattr();
  if (proc == Proc::kFsstat) {
    auto stats = co_await resolved->exp->backend->statfs();
    if (!stats) {
      begin_result(enc, ctx, call, core::to_v3(stats.error(), proc));
      encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
    } else {
      begin_result(enc, ctx, call, Status::kOk);
      encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
      enc.u64(stats->tbytes);
      enc.u64(stats->fbytes);
      enc.u64(stats->abytes);
      enc.u64(stats->tfiles);
      enc.u64(stats->ffiles);
      enc.u64(stats->afiles);
      enc.u32(0);
    }
  } else if (proc == Proc::kFsinfo) {
    core::FsProps fs = core::fs_props(*resolved->exp->backend);
    begin_result(enc, ctx, call, Status::kOk);
    encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
    enc.u32(fs.limits.max_read);
    enc.u32(fs.limits.pref_read);
    enc.u32(4096);
    // A read-only export still advertises legal transfer geometry.  Linux clients consume
    // these values while mounting and expect non-zero multiples; mutations are rejected by
    // ACCESS/procedure policy, not by malformed FSINFO limits.
    enc.u32(fs.limits.max_write);
    enc.u32(fs.limits.pref_write);
    enc.u32(4096);
    enc.u32(fs.limits.pref_readdir);
    enc.u64(fs.limits.max_filesize);
    encode_time(enc, fs.limits.time_delta);
    uint32_t props = fs.kHomogeneous ? kFsfHomogeneous : 0;
    if (fs.link_support) props |= kFsfLink;
    if (fs.symlink_support) props |= kFsfSymlink;
    enc.u32(props);
  } else {
    core::FsProps fs = core::fs_props(*resolved->exp->backend);
    begin_result(enc, ctx, call, Status::kOk);
    encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
    enc.u32(fs.limits.max_link);
    enc.u32(fs.limits.max_name);
    enc.boolean(fs.kNoTrunc);
    enc.boolean(fs.kChownRestricted);
    enc.boolean(fs.case_insensitive);
    enc.boolean(fs.kCasePreserving);
  }
  co_await reply(ctx, enc, cap);
}

rt::Task<void> Engine::proc_commit(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                   Capture* cap) {
  auto args = CommitArgs::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto resolved = co_await resolve(args->file, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!resolved) {
    begin_result(enc, ctx, call, core::to_v3(resolved.error(), Proc::kCommit));
    encode_wcc_none(enc);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  // Flushing does not mutate: shared lock, but the reply still carries wcc_data.
  auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
  auto held = co_await lock->lock_shared();
  core::ChangeSample sample;
  sample.before = co_await core::sample_attr(resolved->obj);
  auto mapped = exports_.squash_cred(rpc_cred, *resolved->exp);
  auto cred = mapped.view();
  backend::OpenCtx open{cred, nullptr};
  auto committed = co_await resolved->obj->commit(open, args->offset, args->count);
  sample.after = co_await core::sample_attr(resolved->obj);
  if (!committed) {
    begin_result(enc, ctx, call, core::to_v3(committed.error(), Proc::kCommit));
    encode_wcc_sample(enc, sample, resolved->exp->fsid);
  } else {
    begin_result(enc, ctx, call, Status::kOk);
    encode_wcc_sample(enc, sample, resolved->exp->fsid);
    enc.opaque_fixed(verf_);
  }
  co_await reply(ctx, enc, cap);
}

// ---- mutating procedures -----------------------------------------------------------
// Every handler follows the core::MutateGuard sequence (plan doc 10 §6.1): resolve ->
// precheck (readonly, names) -> guard.enter (squash, exclusive lock, before sample) ->
// backend op -> guard.finish (after sample).  Failure paths after the lock still carry
// usable WCC data; precheck failures reply with one unlocked sample.

rt::Task<void> Engine::proc_setattr(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                    Capture* cap) {
  auto args = SetattrArgs::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto resolved = co_await resolve(args->object, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!resolved) {
    begin_result(enc, ctx, call, core::to_v3(resolved.error(), Proc::kSetattr));
    encode_wcc_none(enc);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  MutateGuard guard(locks_, exports_, *resolved->exp, rpc_cred);
  if (auto verdict = guard.precheck({}); !verdict) {
    auto attr = co_await core::sample_attr(resolved->obj);
    begin_result(enc, ctx, call, verdict_status(verdict));
    encode_wcc_unchanged(enc, attr, resolved->exp->fsid);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  co_await guard.enter({resolved->obj, resolved->oid});
  const auto& before = guard.first().before;
  if (args->guard && before &&
      (before->ctime.sec != args->guard_ctime.sec ||
       before->ctime.nsec != args->guard_ctime.nsec)) {
    begin_result(enc, ctx, call, Status::kNotSync);
    encode_wcc_unchanged(enc, before, resolved->exp->fsid);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  auto result = co_await resolved->obj->setattr(guard.cred(), args->attrs);
  if (!result) {
    co_await guard.finish();
    begin_result(enc, ctx, call, core::to_v3(result.error(), Proc::kSetattr));
    encode_wcc_sample(enc, guard.first(), resolved->exp->fsid);
  } else {  // setattr returns the post-op attributes: no second sample needed
    begin_result(enc, ctx, call, Status::kOk);
    encode_wcc(enc, wcc_pre(before), *result, resolved->exp->fsid);
  }
  co_await reply(ctx, enc, cap);
}

rt::Task<void> Engine::proc_write(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                  Capture* cap) {
  auto args = WriteArgs::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto resolved = co_await resolve(args->file, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!resolved) {
    begin_result(enc, ctx, call, core::to_v3(resolved.error(), Proc::kWrite));
    encode_wcc_none(enc);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  MutateGuard guard(locks_, exports_, *resolved->exp, rpc_cred);
  if (auto verdict = guard.precheck({}); !verdict) {
    auto attr = co_await core::sample_attr(resolved->obj);
    begin_result(enc, ctx, call, verdict_status(verdict));
    encode_wcc_unchanged(enc, attr, resolved->exp->fsid);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  uint32_t count = std::min<uint32_t>(
      std::min<uint64_t>(args->count, args->data_len),
      resolved->exp->backend->limits().max_write);
  // Per-export QoS (plan doc 10 §4.3), before the exclusive object lock.
  co_await resolved->exp->qos.throttle(true, count);
  co_await guard.enter({resolved->obj, resolved->oid});
  // Hand the payload down as the received segments, truncated to `count` (§2.4).
  SmallVec<iovec, 8> iov;
  for (uint32_t left = count; const auto& seg : args->data) {
    if (left == 0) break;
    uint32_t k = static_cast<uint32_t>(std::min<size_t>(left, seg.size()));
    iov.push_back(iovec{const_cast<std::byte*>(seg.data()), k});
    left -= k;
  }
  backend::OpenCtx open{guard.cred(), nullptr};
  auto written = co_await resolved->obj->write(open, args->offset,
                                               std::span<const iovec>(iov.data(), iov.size()),
                                               stability_from_wire(args->stable));
  co_await guard.finish();
  if (!written) {
    begin_result(enc, ctx, call, core::to_v3(written.error(), Proc::kWrite));
    encode_wcc_sample(enc, guard.first(), resolved->exp->fsid);
  } else {
    begin_result(enc, ctx, call, Status::kOk);
    encode_wcc_sample(enc, guard.first(), resolved->exp->fsid);
    obs::Metrics::instance().write_bytes.fetch_add(*written, std::memory_order_relaxed);
    resolved->exp->metrics.write_bytes.fetch_add(*written, std::memory_order_relaxed);
    resolved->exp->metrics.write_ops.fetch_add(1, std::memory_order_relaxed);
    enc.u32(*written);
    enc.u32(args->stable);  // the backend honored the requested stability exactly
    enc.opaque_fixed(verf_);
  }
  co_await reply(ctx, enc, cap);
}

rt::Task<void> Engine::proc_create(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                   Capture* cap) {
  auto args = CreateArgs::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto dir = co_await resolve(args->where.dir, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!dir) {
    begin_result(enc, ctx, call, core::to_v3(dir.error(), Proc::kCreate));
    encode_wcc_none(enc);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  MutateGuard guard(locks_, exports_, *dir->exp, rpc_cred);
  if (auto verdict = guard.precheck({args->where.name}); !verdict) {
    auto attr = co_await core::sample_attr(dir->obj);
    begin_result(enc, ctx, call, verdict_status(verdict));
    encode_wcc_unchanged(enc, attr, dir->exp->fsid);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  co_await guard.enter({dir->obj, dir->oid});
  const auto& cred = guard.cred();
  auto fail = [&](Status status) {
    begin_result(enc, ctx, call, status);
    encode_wcc_sample(enc, guard.first(), dir->exp->fsid);
  };

  Result<backend::Created> created = Err(errno_from(EIO));
  if (args->mode == kCreateExclusive) {
    created = co_await dir->obj->create(cred, args->where.name, {}, &args->verf);
  } else {
    created = co_await dir->obj->create(cred, args->where.name, args->attrs, nullptr);
    if (!created && created.error() == errno_from(EEXIST) &&
        args->mode == kCreateUnchecked) {
      // UNCHECKED on an existing file succeeds; only a requested size is applied
      // (truncate), matching common server practice.
      auto existing = co_await dir->obj->lookup(cred, args->where.name);
      if (existing) {
        backend::Attr attr{};
        if (args->attrs.size) {
          backend::SetAttr size_only;
          size_only.size = args->attrs.size;
          auto set = co_await (*existing)->setattr(cred, size_only);
          if (!set) {
            co_await guard.finish();
            fail(core::to_v3(set.error(), Proc::kCreate));
            co_await reply(ctx, enc, cap);
            co_return;
          }
          attr = *set;
        } else {
          auto got = co_await (*existing)->getattr();
          if (got) attr = *got;
        }
        created = backend::Created{*existing, attr};
      }
    }
  }

  co_await guard.finish();
  if (!created) {
    fail(core::to_v3(created.error(), Proc::kCreate));
  } else {
    begin_result(enc, ctx, call, Status::kOk);
    FileHandle fh{handles_.encode(*dir->exp, created->obj->id())};
    encode_post_fh(enc, fh);
    encode_post_attr(enc, created->attr, dir->exp->fsid);
    encode_wcc_sample(enc, guard.first(), dir->exp->fsid);
  }
  co_await reply(ctx, enc, cap);
}

rt::Task<void> Engine::proc_mkdir(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                  Capture* cap) {
  auto args = MkdirArgs::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto dir = co_await resolve(args->where.dir, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!dir) {
    begin_result(enc, ctx, call, core::to_v3(dir.error(), Proc::kMkdir));
    encode_wcc_none(enc);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  MutateGuard guard(locks_, exports_, *dir->exp, rpc_cred);
  if (auto verdict = guard.precheck({args->where.name}); !verdict) {
    auto attr = co_await core::sample_attr(dir->obj);
    begin_result(enc, ctx, call, verdict_status(verdict));
    encode_wcc_unchanged(enc, attr, dir->exp->fsid);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  co_await guard.enter({dir->obj, dir->oid});
  auto created = co_await dir->obj->mkdir(guard.cred(), args->where.name, args->attrs);
  co_await guard.finish();
  if (!created) {
    begin_result(enc, ctx, call, core::to_v3(created.error(), Proc::kMkdir));
    encode_wcc_sample(enc, guard.first(), dir->exp->fsid);
  } else {
    begin_result(enc, ctx, call, Status::kOk);
    FileHandle fh{handles_.encode(*dir->exp, created->obj->id())};
    encode_post_fh(enc, fh);
    encode_post_attr(enc, created->attr, dir->exp->fsid);
    encode_wcc_sample(enc, guard.first(), dir->exp->fsid);
  }
  co_await reply(ctx, enc, cap);
}

rt::Task<void> Engine::proc_symlink(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                    Capture* cap) {
  auto args = SymlinkArgs::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto dir = co_await resolve(args->where.dir, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!dir) {
    begin_result(enc, ctx, call, core::to_v3(dir.error(), Proc::kSymlink));
    encode_wcc_none(enc);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  MutateGuard guard(locks_, exports_, *dir->exp, rpc_cred);
  Status precheck = verdict_status(guard.precheck({args->where.name}));
  if (precheck == Status::kOk && !dir->exp->backend->caps().has(backend::Cap::kSymlink))
    precheck = Status::kNotsupp;
  if (precheck != Status::kOk) {
    auto attr = co_await core::sample_attr(dir->obj);
    begin_result(enc, ctx, call, precheck);
    encode_wcc_unchanged(enc, attr, dir->exp->fsid);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  co_await guard.enter({dir->obj, dir->oid});
  auto created = co_await dir->obj->symlink(guard.cred(), args->where.name, args->target,
                                            args->attrs);
  co_await guard.finish();
  if (!created) {
    begin_result(enc, ctx, call, core::to_v3(created.error(), Proc::kSymlink));
    encode_wcc_sample(enc, guard.first(), dir->exp->fsid);
  } else {
    begin_result(enc, ctx, call, Status::kOk);
    FileHandle fh{handles_.encode(*dir->exp, created->obj->id())};
    encode_post_fh(enc, fh);
    encode_post_attr(enc, created->attr, dir->exp->fsid);
    encode_wcc_sample(enc, guard.first(), dir->exp->fsid);
  }
  co_await reply(ctx, enc, cap);
}

rt::Task<void> Engine::proc_mknod(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                  Capture* cap) {
  auto args = MknodArgs::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto dir = co_await resolve(args->where.dir, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!dir) {
    begin_result(enc, ctx, call, core::to_v3(dir.error(), Proc::kMknod));
    encode_wcc_none(enc);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  MutateGuard guard(locks_, exports_, *dir->exp, rpc_cred);
  bool device = args->type == backend::FType::kChr || args->type == backend::FType::kBlk;
  bool special = device || args->type == backend::FType::kSock ||
                 args->type == backend::FType::kFifo;
  Status precheck = verdict_status(guard.precheck({args->where.name}));
  if (precheck == Status::kOk) {
    if (!special) precheck = Status::kBadtype;
    else if (!dir->exp->backend->caps().has(backend::Cap::kMknod))
      precheck = Status::kNotsupp;
  }
  if (precheck != Status::kOk) {
    auto attr = co_await core::sample_attr(dir->obj);
    begin_result(enc, ctx, call, precheck);
    encode_wcc_unchanged(enc, attr, dir->exp->fsid);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  co_await guard.enter({dir->obj, dir->oid});
  auto created = co_await dir->obj->mknod(guard.cred(), args->where.name, args->type,
                                          args->dev, args->attrs);
  co_await guard.finish();
  if (!created) {
    begin_result(enc, ctx, call, core::to_v3(created.error(), Proc::kMknod));
    encode_wcc_sample(enc, guard.first(), dir->exp->fsid);
  } else {
    begin_result(enc, ctx, call, Status::kOk);
    FileHandle fh{handles_.encode(*dir->exp, created->obj->id())};
    encode_post_fh(enc, fh);
    encode_post_attr(enc, created->attr, dir->exp->fsid);
    encode_wcc_sample(enc, guard.first(), dir->exp->fsid);
  }
  co_await reply(ctx, enc, cap);
}

rt::Task<void> Engine::proc_remove(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                   Capture* cap) {
  auto args = Diropargs::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto dir = co_await resolve(args->dir, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!dir) {
    begin_result(enc, ctx, call, core::to_v3(dir.error(), Proc::kRemove));
    encode_wcc_none(enc);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  MutateGuard guard(locks_, exports_, *dir->exp, rpc_cred);
  if (auto verdict = guard.precheck({args->name}); !verdict) {
    auto attr = co_await core::sample_attr(dir->obj);
    begin_result(enc, ctx, call, verdict_status(verdict));
    encode_wcc_unchanged(enc, attr, dir->exp->fsid);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  co_await guard.enter({dir->obj, dir->oid});
  auto removed = co_await dir->obj->unlink(guard.cred(), args->name);
  co_await guard.finish();
  begin_result(enc, ctx, call,
               removed ? Status::kOk : core::to_v3(removed.error(), Proc::kRemove));
  encode_wcc_sample(enc, guard.first(), dir->exp->fsid);
  co_await reply(ctx, enc, cap);
}

rt::Task<void> Engine::proc_rmdir(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                  Capture* cap) {
  auto args = Diropargs::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto dir = co_await resolve(args->dir, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!dir) {
    begin_result(enc, ctx, call, core::to_v3(dir.error(), Proc::kRmdir));
    encode_wcc_none(enc);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  MutateGuard guard(locks_, exports_, *dir->exp, rpc_cred);
  if (auto verdict = guard.precheck({args->name}); !verdict) {
    Status status = verdict_status(verdict);
    if (verdict.name == core::NameCheck::kDot)  // RFC 1813 §3.3.13
      status = args->name == "." ? Status::kInval : Status::kExist;
    auto attr = co_await core::sample_attr(dir->obj);
    begin_result(enc, ctx, call, status);
    encode_wcc_unchanged(enc, attr, dir->exp->fsid);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  co_await guard.enter({dir->obj, dir->oid});
  auto removed = co_await dir->obj->rmdir(guard.cred(), args->name);
  co_await guard.finish();
  begin_result(enc, ctx, call,
               removed ? Status::kOk : core::to_v3(removed.error(), Proc::kRmdir));
  encode_wcc_sample(enc, guard.first(), dir->exp->fsid);
  co_await reply(ctx, enc, cap);
}

rt::Task<void> Engine::proc_rename(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                   Capture* cap) {
  auto args = RenameArgs::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto from = co_await resolve(args->from.dir, ctx.peer.addr);
  auto to_res = from ? co_await resolve(args->to.dir, ctx.peer.addr)
                     : Result<Resolved>(Err(errno_from(ESTALE)));
  xdr::XdrEnc enc(ctx.pool);
  if (!from || !to_res) {
    begin_result(enc, ctx, call,
                 core::to_v3(!from ? from.error() : to_res.error(), Proc::kRename));
    encode_wcc_none(enc);
    encode_wcc_none(enc);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  auto& to = *to_res;
  // Cross-export rename is not expressible for the backend: intercept as XDEV.
  if (from->exp != to.exp) {
    begin_result(enc, ctx, call, Status::kXdev);
    encode_wcc_none(enc);
    encode_wcc_none(enc);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  MutateGuard guard(locks_, exports_, *from->exp, rpc_cred);
  if (auto verdict = guard.precheck({args->from.name, args->to.name}); !verdict) {
    auto attr_from = co_await core::sample_attr(from->obj);
    auto attr_to = from->oid == to.oid ? attr_from : co_await core::sample_attr(to.obj);
    begin_result(enc, ctx, call, verdict_status(verdict));
    encode_wcc_unchanged(enc, attr_from, from->exp->fsid);
    encode_wcc_unchanged(enc, attr_to, to.exp->fsid);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  // Two-directory lock ordering by ObjId happens inside the guard (design 04 §4.2).
  co_await guard.enter({from->obj, from->oid}, {to.obj, to.oid});
  auto renamed = co_await from->obj->rename(guard.cred(), args->from.name, *to.obj,
                                            args->to.name);
  co_await guard.finish();
  begin_result(enc, ctx, call,
               renamed ? Status::kOk : core::to_v3(renamed.error(), Proc::kRename));
  encode_wcc_sample(enc, guard.first(), from->exp->fsid);
  encode_wcc_sample(enc, guard.second(), to.exp->fsid);
  co_await reply(ctx, enc, cap);
}

rt::Task<void> Engine::proc_link(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                 Capture* cap) {
  auto args = LinkArgs::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto file = co_await resolve(args->file, ctx.peer.addr);
  auto dir_res = file ? co_await resolve(args->to.dir, ctx.peer.addr)
                      : Result<Resolved>(Err(errno_from(ESTALE)));
  xdr::XdrEnc enc(ctx.pool);
  if (!file || !dir_res) {
    begin_result(enc, ctx, call,
                 core::to_v3(!file ? file.error() : dir_res.error(), Proc::kLink));
    encode_post_attr(enc, std::nullopt, 0);
    encode_wcc_none(enc);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  auto& dir = *dir_res;
  if (file->exp != dir.exp) {
    begin_result(enc, ctx, call, Status::kXdev);
    encode_post_attr(enc, std::nullopt, 0);
    encode_wcc_none(enc);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  MutateGuard guard(locks_, exports_, *dir.exp, rpc_cred);
  Status precheck = verdict_status(guard.precheck({args->to.name}));
  if (precheck == Status::kOk && !dir.exp->backend->caps().has(backend::Cap::kHardlink))
    precheck = Status::kNotsupp;
  if (precheck != Status::kOk) {
    auto file_attr = co_await core::sample_attr(file->obj);
    auto dir_attr = co_await core::sample_attr(dir.obj);
    begin_result(enc, ctx, call, precheck);
    encode_post_attr(enc, file_attr, file->exp->fsid);
    encode_wcc_unchanged(enc, dir_attr, dir.exp->fsid);
    co_await reply(ctx, enc, cap);
    co_return;
  }
  // The directory carries wcc_data; the file only needs post-op attributes, so it is
  // locked but not sampled up front.
  co_await guard.enter({dir.obj, dir.oid}, {file->obj, file->oid, /*sample=*/false});
  auto linked = co_await dir.obj->link(guard.cred(), *file->obj, args->to.name);
  auto file_attr = co_await core::sample_attr(file->obj);
  co_await guard.finish();
  begin_result(enc, ctx, call,
               linked ? Status::kOk : core::to_v3(linked.error(), Proc::kLink));
  encode_post_attr(enc, file_attr, file->exp->fsid);
  encode_wcc_sample(enc, guard.first(), dir.exp->fsid);
  co_await reply(ctx, enc, cap);
}

}  // namespace lnfs::nfsv3
