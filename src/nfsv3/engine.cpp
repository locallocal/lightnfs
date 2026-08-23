#include "nfsv3/engine.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>

#include <chrono>

#include "core/errmap.hpp"
#include "core/readdir.hpp"
#include "obs/errlog.hpp"
#include "obs/metrics.hpp"
#include "rpc/drc.hpp"
#include "util/log.hpp"
#include "transport/connection.hpp"

namespace lnfs::nfsv3 {
namespace {

using rpc::RpcCall;
using transport::ConnCtx;

rt::Task<void> send(ConnCtx& ctx, xdr::XdrEnc& enc) { co_await ctx.send(enc.take()); }

// Send + optionally capture the reply bytes for the DRC (design 03 §3.7).
rt::Task<void> send_captured(ConnCtx& ctx, xdr::XdrEnc& enc,
                             std::vector<std::byte>* cap) {
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
// exception paths included.
struct ProcTimer {
  explicit ProcTimer(uint32_t p) : proc(p % obs::Metrics::kV3Procs) {}
  ~ProcTimer() {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    auto& m = obs::Metrics::instance();
    m.v3_calls[proc].fetch_add(1, std::memory_order_relaxed);
    m.v3_duration_us[proc].fetch_add(static_cast<uint64_t>(us), std::memory_order_relaxed);
  }
  uint32_t proc;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
};

bool valid_component(std::string_view name) {  // LOOKUP may name "." / ".."
  return !name.empty() && name.size() <= kMaxName &&
         name.find('/') == std::string_view::npos &&
         name.find('\0') == std::string_view::npos;
}

bool valid_new_name(std::string_view name) {  // creation family must not
  return valid_component(name) && name != "." && name != "..";
}

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

}  // namespace

void Engine::register_with(rpc::Dispatcher& dispatcher) {
  dispatcher.add({kProgram, kVersion, kVersion,
                  [this](ConnCtx& ctx, RpcCall& call, const rpc::Cred& cred) {
                    return dispatch(ctx, call, cred);
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
  ProcTimer timer(call.proc);
  if (call.proc > static_cast<uint32_t>(Proc::kCommit)) {
    xdr::XdrEnc enc(ctx.pool);
    rpc::encode_reply_accepted_err(enc, call.xid, rpc::kProcUnavail);
    co_await send(ctx, enc);
    co_return;
  }
  Proc proc = static_cast<Proc>(call.proc);
  if (proc == Proc::kNull) {
    xdr::XdrEnc enc(ctx.pool);
    rpc::encode_reply_success(enc, call.xid);
    co_await send(ctx, enc);
    co_return;
  }

  if (drc_ && drc_cached(proc)) {
    rpc::Drc::Key key{ctx.peer.to_string(), call.xid, call.prog, call.vers, call.proc,
                      call.args_hash};
    auto claim = co_await drc_->begin(key);
    if (!claim.owner) {  // retransmission: replay the cached reply bytes verbatim
      xdr::XdrEnc enc(ctx.pool);
      enc.opaque_fixed(claim.cached);
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
    // the claim so a waiting retransmission re-executes instead of hanging.
    if (!error && !cap.empty()) co_await drc_->complete(key, std::move(cap));
    else co_await drc_->abort(key);
    if (error) std::rethrow_exception(error);
    co_return;
  }
  co_await dispatch_proc(ctx, call, rpc_cred, nullptr);
}

rt::Task<void> Engine::dispatch_proc(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred,
                                     Capture* cap) {
  Proc proc = static_cast<Proc>(call.proc);

  if (proc == Proc::kSetattr) co_return co_await proc_setattr(ctx, call, rpc_cred, cap);
  if (proc == Proc::kWrite) co_return co_await proc_write(ctx, call, rpc_cred);
  if (proc == Proc::kCreate) co_return co_await proc_create(ctx, call, rpc_cred, cap);
  if (proc == Proc::kMkdir) co_return co_await proc_mkdir(ctx, call, rpc_cred, cap);
  if (proc == Proc::kSymlink) co_return co_await proc_symlink(ctx, call, rpc_cred, cap);
  if (proc == Proc::kMknod) co_return co_await proc_mknod(ctx, call, rpc_cred, cap);
  if (proc == Proc::kRemove) co_return co_await proc_remove(ctx, call, rpc_cred, cap);
  if (proc == Proc::kRmdir) co_return co_await proc_rmdir(ctx, call, rpc_cred, cap);
  if (proc == Proc::kRename) co_return co_await proc_rename(ctx, call, rpc_cred, cap);
  if (proc == Proc::kLink) co_return co_await proc_link(ctx, call, rpc_cred, cap);
  if (proc == Proc::kCommit) co_return co_await proc_commit(ctx, call, rpc_cred);

  if (proc == Proc::kGetattr) {
    auto args = FileHandle::decode(call.args);
    if (!args || !call.args.at_end()) {
      co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
      co_return;
    }
    auto resolved = co_await resolve(*args, ctx.peer.addr);
    xdr::XdrEnc enc(ctx.pool);
    if (!resolved) {
      begin_result(enc, ctx, call, core::to_v3(resolved.error(), proc));
    } else {
      auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
      auto held = co_await lock->lock_shared();
      auto attr = co_await resolved->obj->getattr();
      if (!attr) begin_result(enc, ctx, call, core::to_v3(attr.error(), proc));
      else {
        begin_result(enc, ctx, call, Status::kOk);
        encode_fattr(enc, *attr, resolved->exp->fsid);
      }
    }
    co_await send(ctx, enc);
    co_return;
  }

  if (proc == Proc::kLookup) {
    auto args = Diropargs::decode(call.args);
    if (!args || !call.args.at_end() || !valid_component(args->name)) {
      co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
      co_return;
    }
    auto dir = co_await resolve(args->dir, ctx.peer.addr);
    xdr::XdrEnc enc(ctx.pool);
    if (!dir) {
      begin_result(enc, ctx, call, core::to_v3(dir.error(), proc));
      encode_post_attr(enc, std::nullopt, 0);
      co_await send(ctx, enc);
      co_return;
    }
    auto mapped = exports_.squash_cred(rpc_cred, *dir->exp);
    auto cred = mapped.view();
    auto lock = locks_.get(dir->exp->fsid, dir->oid);
    auto held = co_await lock->lock_shared();
    auto found = co_await dir->obj->lookup(cred, args->name);
    auto dir_attr = co_await dir->obj->getattr();
    if (!found) {
      begin_result(enc, ctx, call, core::to_v3(found.error(), proc));
      encode_post_attr(enc, attr_value(dir_attr), dir->exp->fsid);
    } else {
      auto obj_attr = co_await (*found)->getattr();
      FileHandle wire{handles_.encode(*dir->exp, (*found)->id())};
      begin_result(enc, ctx, call, Status::kOk);
      wire.encode(enc);
      encode_post_attr(enc, attr_value(obj_attr), dir->exp->fsid);
      encode_post_attr(enc, attr_value(dir_attr), dir->exp->fsid);
    }
    co_await send(ctx, enc);
    co_return;
  }

  if (proc == Proc::kAccess) {
    auto args = AccessArgs::decode(call.args);
    if (!args || !call.args.at_end()) {
      co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
      co_return;
    }
    auto resolved = co_await resolve(args->object, ctx.peer.addr);
    xdr::XdrEnc enc(ctx.pool);
    if (!resolved) {
      begin_result(enc, ctx, call, core::to_v3(resolved.error(), proc));
      encode_post_attr(enc, std::nullopt, 0);
    } else {
      auto mapped = exports_.squash_cred(rpc_cred, *resolved->exp);
      auto cred = mapped.view();
      auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
      auto held = co_await lock->lock_shared();
      auto allowed = co_await resolved->obj->access(cred, access_from_wire(args->access));
      auto attr = co_await resolved->obj->getattr();
      if (!allowed) {
        begin_result(enc, ctx, call, core::to_v3(allowed.error(), proc));
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
    co_await send(ctx, enc);
    co_return;
  }

  if (proc == Proc::kReadlink) {
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
    } else {
      auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
      auto held = co_await lock->lock_shared();
      auto target = co_await resolved->obj->readlink();
      auto attr = co_await resolved->obj->getattr();
      if (!target) {
        begin_result(enc, ctx, call, core::to_v3(target.error(), proc));
        encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
      } else {
        begin_result(enc, ctx, call, Status::kOk);
        encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
        enc.string(*target);
      }
    }
    co_await send(ctx, enc);
    co_return;
  }

  if (proc == Proc::kRead) {
    auto args = ReadArgs::decode(call.args);
    if (!args || !call.args.at_end()) {
      co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
      co_return;
    }
    auto resolved = co_await resolve(args->file, ctx.peer.addr);
    xdr::XdrEnc enc(ctx.pool);
    if (!resolved) {
      begin_result(enc, ctx, call, core::to_v3(resolved.error(), proc));
      encode_post_attr(enc, std::nullopt, 0);
      co_await send(ctx, enc);
      co_return;
    }
    auto mapped = exports_.squash_cred(rpc_cred, *resolved->exp);
    auto cred = mapped.view();
    uint32_t count = std::min(args->count, resolved->exp->backend->limits().max_read);
    auto data = ctx.pool.alloc(std::max<uint32_t>(count, 1));
    bool eof = false;
    auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
    auto held = co_await lock->lock_shared();
    backend::OpenCtx open{cred, nullptr};
    auto read = co_await resolved->obj->read(open, args->offset,
                                             std::span<std::byte>(data.data(), count), eof);
    auto attr = co_await resolved->obj->getattr();
    if (!read) {
      begin_result(enc, ctx, call, core::to_v3(read.error(), proc));
      encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
    } else {
      begin_result(enc, ctx, call, Status::kOk);
      encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
      obs::Metrics::instance().read_bytes.fetch_add(*read, std::memory_order_relaxed);
      enc.u32(*read);
      enc.boolean(eof);
      enc.u32(*read);
      enc.attach(std::move(data), 0, *read);
    }
    co_await send(ctx, enc);
    co_return;
  }

  if (proc == Proc::kReaddir || proc == Proc::kReaddirplus) {
    bool plus = proc == Proc::kReaddirplus;
    FileHandle dir_fh;
    uint64_t cookie = 0;
    uint32_t dircount = 0, maxcount = 0;
    bool bad_verifier = false;
    if (plus) {
      auto args = ReaddirPlusArgs::decode(call.args);
      if (!args || !call.args.at_end()) {
        co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
        co_return;
      }
      dir_fh = std::move(args->dir);
      cookie = args->cookie;
      bad_verifier = cookie != 0 &&
                     std::any_of(args->cookieverf.begin(), args->cookieverf.end(),
                                 [](std::byte value) { return value != std::byte{0}; });
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
      bad_verifier = cookie != 0 &&
                     std::any_of(args->cookieverf.begin(), args->cookieverf.end(),
                                 [](std::byte value) { return value != std::byte{0}; });
      dircount = maxcount = args->count;
    }
    auto resolved = co_await resolve(dir_fh, ctx.peer.addr);
    xdr::XdrEnc enc(ctx.pool);
    if (!resolved) {
      begin_result(enc, ctx, call, core::to_v3(resolved.error(), proc));
      encode_post_attr(enc, std::nullopt, 0);
      co_await send(ctx, enc);
      co_return;
    }
    auto mapped = exports_.squash_cred(rpc_cred, *resolved->exp);
    auto cred = mapped.view();
    auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
    auto held = co_await lock->lock_shared();
    auto attr = co_await resolved->obj->getattr();
    if (bad_verifier) {
      begin_result(enc, ctx, call, Status::kBadCookie);
      encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
      co_await send(ctx, enc);
      co_return;
    }
    constexpr size_t kFixedReply = 24 + 4 + 88 + 8 + 8;
    if (maxcount < kFixedReply || dircount < 24) {
      begin_result(enc, ctx, call, Status::kToosmall);
      encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
      co_await send(ctx, enc);
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
      co_await send(ctx, enc);
      co_return;
    }
    begin_result(enc, ctx, call, Status::kOk);
    encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
    std::array<std::byte, 8> zero_verf{};
    enc.opaque_fixed(zero_verf);
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
    co_await send(ctx, enc);
    co_return;
  }

  // FSSTAT / FSINFO / PATHCONF all take one file handle and return post-op attributes.
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
    co_await send(ctx, enc);
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
    auto limits = resolved->exp->backend->limits();
    auto caps = resolved->exp->backend->caps();
    begin_result(enc, ctx, call, Status::kOk);
    encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
    enc.u32(limits.max_read);
    enc.u32(std::min(limits.pref_read, limits.max_read));
    enc.u32(4096);
    // A read-only export still advertises legal transfer geometry.  Linux clients consume
    // these values while mounting and expect non-zero multiples; mutations are rejected by
    // ACCESS/procedure policy, not by malformed FSINFO limits.
    enc.u32(limits.max_write);
    enc.u32(std::min(limits.pref_write, limits.max_write));
    enc.u32(4096);
    enc.u32(limits.pref_readdir);
    enc.u64(limits.max_filesize);
    encode_time(enc, limits.time_delta);
    uint32_t props = kFsfHomogeneous;
    if (caps.has(backend::Cap::kHardlink)) props |= kFsfLink;
    if (caps.has(backend::Cap::kSymlink)) props |= kFsfSymlink;
    enc.u32(props);
  } else {
    auto limits = resolved->exp->backend->limits();
    auto caps = resolved->exp->backend->caps();
    begin_result(enc, ctx, call, Status::kOk);
    encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
    enc.u32(limits.max_link);
    enc.u32(limits.max_name);
    enc.boolean(true);
    enc.boolean(true);
    enc.boolean(caps.has(backend::Cap::kCaseInsensitive));
    enc.boolean(true);
  }
  co_await send(ctx, enc);
}

// ---- phase-2 write procedures ---------------------------------------------
// Shared shape (core mutate template, design 04 §4.2): resolve -> ROFS precheck ->
// exclusive object lock -> sample before -> backend op -> sample after.  Failure paths
// sample `after` too so every branch carries usable WCC data.

namespace {

rt::Task<std::optional<backend::Attr>> sample(const backend::ObjPtr& obj) {
  auto attr = co_await obj->getattr();
  co_return attr ? std::optional<backend::Attr>(*attr) : std::nullopt;
}

backend::Stability stability_from_wire(uint32_t stable) {
  switch (stable) {
    case kUnstable: return backend::Stability::kUnstable;
    case kDataSync: return backend::Stability::kDataSync;
    default: return backend::Stability::kFileSync;
  }
}

}  // namespace

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
    encode_wcc(enc, std::nullopt, std::nullopt, 0);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
  auto held = co_await lock->lock();
  auto before = co_await sample(resolved->obj);
  if (resolved->exp->readonly) {
    begin_result(enc, ctx, call, Status::kRofs);
    encode_wcc(enc, wcc_pre(before), before, resolved->exp->fsid);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  if (args->guard && before &&
      (before->ctime.sec != args->guard_ctime.sec ||
       before->ctime.nsec != args->guard_ctime.nsec)) {
    begin_result(enc, ctx, call, Status::kNotSync);
    encode_wcc(enc, wcc_pre(before), before, resolved->exp->fsid);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto mapped = exports_.squash_cred(rpc_cred, *resolved->exp);
  auto cred = mapped.view();
  auto result = co_await resolved->obj->setattr(cred, args->attrs);
  if (!result) {
    auto after = co_await sample(resolved->obj);
    begin_result(enc, ctx, call, core::to_v3(result.error(), Proc::kSetattr));
    encode_wcc(enc, wcc_pre(before), after, resolved->exp->fsid);
  } else {
    begin_result(enc, ctx, call, Status::kOk);
    encode_wcc(enc, wcc_pre(before), *result, resolved->exp->fsid);
  }
  co_await send_captured(ctx, enc, cap);
}

rt::Task<void> Engine::proc_write(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred) {
  auto args = WriteArgs::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto resolved = co_await resolve(args->file, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!resolved) {
    begin_result(enc, ctx, call, core::to_v3(resolved.error(), Proc::kWrite));
    encode_wcc(enc, std::nullopt, std::nullopt, 0);
    co_await send(ctx, enc);
    co_return;
  }
  auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
  auto held = co_await lock->lock();
  auto before = co_await sample(resolved->obj);
  if (resolved->exp->readonly) {
    begin_result(enc, ctx, call, Status::kRofs);
    encode_wcc(enc, wcc_pre(before), before, resolved->exp->fsid);
    co_await send(ctx, enc);
    co_return;
  }
  auto mapped = exports_.squash_cred(rpc_cred, *resolved->exp);
  auto cred = mapped.view();
  uint32_t count = std::min<uint32_t>(
      std::min<uint64_t>(args->count, args->data.size()),
      resolved->exp->backend->limits().max_write);
  backend::OpenCtx open{cred, nullptr};
  auto written = co_await resolved->obj->write(open, args->offset,
                                               args->data.first(count),
                                               stability_from_wire(args->stable));
  auto after = co_await sample(resolved->obj);
  if (!written) {
    begin_result(enc, ctx, call, core::to_v3(written.error(), Proc::kWrite));
    encode_wcc(enc, wcc_pre(before), after, resolved->exp->fsid);
  } else {
    begin_result(enc, ctx, call, Status::kOk);
    encode_wcc(enc, wcc_pre(before), after, resolved->exp->fsid);
    obs::Metrics::instance().write_bytes.fetch_add(*written, std::memory_order_relaxed);
    enc.u32(*written);
    enc.u32(args->stable);  // the backend honored the requested stability exactly
    enc.opaque_fixed(verf_);
  }
  co_await send(ctx, enc);
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
    encode_wcc(enc, std::nullopt, std::nullopt, 0);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto lock = locks_.get(dir->exp->fsid, dir->oid);
  auto held = co_await lock->lock();
  auto before = co_await sample(dir->obj);
  auto fail = [&](Status status, const std::optional<backend::Attr>& after) {
    begin_result(enc, ctx, call, status);
    encode_wcc(enc, wcc_pre(before), after, dir->exp->fsid);
  };
  if (dir->exp->readonly) {
    fail(Status::kRofs, before);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  if (!valid_new_name(args->where.name)) {
    fail(Status::kAcces, before);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto mapped = exports_.squash_cred(rpc_cred, *dir->exp);
  auto cred = mapped.view();

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
            auto after = co_await sample(dir->obj);
            fail(core::to_v3(set.error(), Proc::kCreate), after);
            co_await send_captured(ctx, enc, cap);
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

  auto after = co_await sample(dir->obj);
  if (!created) {
    fail(core::to_v3(created.error(), Proc::kCreate), after);
  } else {
    begin_result(enc, ctx, call, Status::kOk);
    FileHandle fh{handles_.encode(*dir->exp, created->obj->id())};
    encode_post_fh(enc, fh);
    encode_post_attr(enc, created->attr, dir->exp->fsid);
    encode_wcc(enc, wcc_pre(before), after, dir->exp->fsid);
  }
  co_await send_captured(ctx, enc, cap);
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
    encode_wcc(enc, std::nullopt, std::nullopt, 0);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto lock = locks_.get(dir->exp->fsid, dir->oid);
  auto held = co_await lock->lock();
  auto before = co_await sample(dir->obj);
  Status precheck = Status::kOk;
  if (dir->exp->readonly) precheck = Status::kRofs;
  else if (!valid_new_name(args->where.name)) precheck = Status::kAcces;
  if (precheck != Status::kOk) {
    begin_result(enc, ctx, call, precheck);
    encode_wcc(enc, wcc_pre(before), before, dir->exp->fsid);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto mapped = exports_.squash_cred(rpc_cred, *dir->exp);
  auto cred = mapped.view();
  auto created = co_await dir->obj->mkdir(cred, args->where.name, args->attrs);
  auto after = co_await sample(dir->obj);
  if (!created) {
    begin_result(enc, ctx, call, core::to_v3(created.error(), Proc::kMkdir));
    encode_wcc(enc, wcc_pre(before), after, dir->exp->fsid);
  } else {
    begin_result(enc, ctx, call, Status::kOk);
    FileHandle fh{handles_.encode(*dir->exp, created->obj->id())};
    encode_post_fh(enc, fh);
    encode_post_attr(enc, created->attr, dir->exp->fsid);
    encode_wcc(enc, wcc_pre(before), after, dir->exp->fsid);
  }
  co_await send_captured(ctx, enc, cap);
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
    encode_wcc(enc, std::nullopt, std::nullopt, 0);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto lock = locks_.get(dir->exp->fsid, dir->oid);
  auto held = co_await lock->lock();
  auto before = co_await sample(dir->obj);
  Status precheck = Status::kOk;
  if (dir->exp->readonly) precheck = Status::kRofs;
  else if (!dir->exp->backend->caps().has(backend::Cap::kSymlink))
    precheck = Status::kNotsupp;
  else if (!valid_new_name(args->where.name)) precheck = Status::kAcces;
  if (precheck != Status::kOk) {
    begin_result(enc, ctx, call, precheck);
    encode_wcc(enc, wcc_pre(before), before, dir->exp->fsid);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto mapped = exports_.squash_cred(rpc_cred, *dir->exp);
  auto cred = mapped.view();
  auto created = co_await dir->obj->symlink(cred, args->where.name, args->target,
                                            args->attrs);
  auto after = co_await sample(dir->obj);
  if (!created) {
    begin_result(enc, ctx, call, core::to_v3(created.error(), Proc::kSymlink));
    encode_wcc(enc, wcc_pre(before), after, dir->exp->fsid);
  } else {
    begin_result(enc, ctx, call, Status::kOk);
    FileHandle fh{handles_.encode(*dir->exp, created->obj->id())};
    encode_post_fh(enc, fh);
    encode_post_attr(enc, created->attr, dir->exp->fsid);
    encode_wcc(enc, wcc_pre(before), after, dir->exp->fsid);
  }
  co_await send_captured(ctx, enc, cap);
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
    encode_wcc(enc, std::nullopt, std::nullopt, 0);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto lock = locks_.get(dir->exp->fsid, dir->oid);
  auto held = co_await lock->lock();
  auto before = co_await sample(dir->obj);
  Status precheck = Status::kOk;
  bool device = args->type == backend::FType::kChr || args->type == backend::FType::kBlk;
  bool special = device || args->type == backend::FType::kSock ||
                 args->type == backend::FType::kFifo;
  if (dir->exp->readonly) precheck = Status::kRofs;
  else if (!special) precheck = Status::kBadtype;
  else if (!dir->exp->backend->caps().has(backend::Cap::kMknod))
    precheck = Status::kNotsupp;
  else if (!valid_new_name(args->where.name)) precheck = Status::kAcces;
  if (precheck != Status::kOk) {
    begin_result(enc, ctx, call, precheck);
    encode_wcc(enc, wcc_pre(before), before, dir->exp->fsid);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto mapped = exports_.squash_cred(rpc_cred, *dir->exp);
  auto cred = mapped.view();
  auto created = co_await dir->obj->mknod(cred, args->where.name, args->type, args->dev,
                                          args->attrs);
  auto after = co_await sample(dir->obj);
  if (!created) {
    begin_result(enc, ctx, call, core::to_v3(created.error(), Proc::kMknod));
    encode_wcc(enc, wcc_pre(before), after, dir->exp->fsid);
  } else {
    begin_result(enc, ctx, call, Status::kOk);
    FileHandle fh{handles_.encode(*dir->exp, created->obj->id())};
    encode_post_fh(enc, fh);
    encode_post_attr(enc, created->attr, dir->exp->fsid);
    encode_wcc(enc, wcc_pre(before), after, dir->exp->fsid);
  }
  co_await send_captured(ctx, enc, cap);
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
    encode_wcc(enc, std::nullopt, std::nullopt, 0);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto lock = locks_.get(dir->exp->fsid, dir->oid);
  auto held = co_await lock->lock();
  auto before = co_await sample(dir->obj);
  Status precheck = Status::kOk;
  if (dir->exp->readonly) precheck = Status::kRofs;
  else if (!valid_new_name(args->name)) precheck = Status::kAcces;
  if (precheck != Status::kOk) {
    begin_result(enc, ctx, call, precheck);
    encode_wcc(enc, wcc_pre(before), before, dir->exp->fsid);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto mapped = exports_.squash_cred(rpc_cred, *dir->exp);
  auto cred = mapped.view();
  auto removed = co_await dir->obj->unlink(cred, args->name);
  auto after = co_await sample(dir->obj);
  begin_result(enc, ctx, call,
               removed ? Status::kOk : core::to_v3(removed.error(), Proc::kRemove));
  encode_wcc(enc, wcc_pre(before), after, dir->exp->fsid);
  co_await send_captured(ctx, enc, cap);
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
    encode_wcc(enc, std::nullopt, std::nullopt, 0);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto lock = locks_.get(dir->exp->fsid, dir->oid);
  auto held = co_await lock->lock();
  auto before = co_await sample(dir->obj);
  Status precheck = Status::kOk;
  if (dir->exp->readonly) precheck = Status::kRofs;
  else if (args->name == ".") precheck = Status::kInval;
  else if (args->name == "..") precheck = Status::kExist;
  else if (!valid_new_name(args->name)) precheck = Status::kAcces;
  if (precheck != Status::kOk) {
    begin_result(enc, ctx, call, precheck);
    encode_wcc(enc, wcc_pre(before), before, dir->exp->fsid);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto mapped = exports_.squash_cred(rpc_cred, *dir->exp);
  auto cred = mapped.view();
  auto removed = co_await dir->obj->rmdir(cred, args->name);
  auto after = co_await sample(dir->obj);
  begin_result(enc, ctx, call,
               removed ? Status::kOk : core::to_v3(removed.error(), Proc::kRmdir));
  encode_wcc(enc, wcc_pre(before), after, dir->exp->fsid);
  co_await send_captured(ctx, enc, cap);
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
    encode_wcc(enc, std::nullopt, std::nullopt, 0);
    encode_wcc(enc, std::nullopt, std::nullopt, 0);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto& to = *to_res;
  // Cross-export rename is not expressible for the backend: intercept as XDEV.
  if (from->exp != to.exp) {
    begin_result(enc, ctx, call, Status::kXdev);
    encode_wcc(enc, std::nullopt, std::nullopt, 0);
    encode_wcc(enc, std::nullopt, std::nullopt, 0);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  // Two-directory lock ordering by ObjId (design 04 §4.2).
  auto lock_a = locks_.get(from->exp->fsid, from->oid);
  auto lock_b = locks_.get(to.exp->fsid, to.oid);
  bool same = lock_a.get() == lock_b.get();
  if (!same && to.oid < from->oid) std::swap(lock_a, lock_b);
  auto held_a = co_await lock_a->lock();
  std::optional<decltype(held_a)> held_b;
  if (!same) held_b.emplace(co_await lock_b->lock());

  auto before_from = co_await sample(from->obj);
  auto before_to = same ? before_from : co_await sample(to.obj);
  auto fail = [&](Status status, const std::optional<backend::Attr>& after_from,
                  const std::optional<backend::Attr>& after_to) {
    begin_result(enc, ctx, call, status);
    encode_wcc(enc, wcc_pre(before_from), after_from, from->exp->fsid);
    encode_wcc(enc, wcc_pre(before_to), after_to, to.exp->fsid);
  };
  Status precheck = Status::kOk;
  if (from->exp->readonly) precheck = Status::kRofs;
  else if (!valid_new_name(args->from.name) || !valid_new_name(args->to.name))
    precheck = Status::kAcces;
  if (precheck != Status::kOk) {
    fail(precheck, before_from, before_to);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto mapped = exports_.squash_cred(rpc_cred, *from->exp);
  auto cred = mapped.view();
  auto renamed = co_await from->obj->rename(cred, args->from.name, *to.obj,
                                            args->to.name);
  auto after_from = co_await sample(from->obj);
  auto after_to = same ? after_from : co_await sample(to.obj);
  if (!renamed) {
    fail(core::to_v3(renamed.error(), Proc::kRename), after_from, after_to);
  } else {
    begin_result(enc, ctx, call, Status::kOk);
    encode_wcc(enc, wcc_pre(before_from), after_from, from->exp->fsid);
    encode_wcc(enc, wcc_pre(before_to), after_to, to.exp->fsid);
  }
  co_await send_captured(ctx, enc, cap);
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
    encode_wcc(enc, std::nullopt, std::nullopt, 0);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto& dir = *dir_res;
  if (file->exp != dir.exp) {
    begin_result(enc, ctx, call, Status::kXdev);
    encode_post_attr(enc, std::nullopt, 0);
    encode_wcc(enc, std::nullopt, std::nullopt, 0);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto lock_a = locks_.get(file->exp->fsid, file->oid);
  auto lock_b = locks_.get(dir.exp->fsid, dir.oid);
  bool same = lock_a.get() == lock_b.get();
  if (!same && dir.oid < file->oid) std::swap(lock_a, lock_b);
  auto held_a = co_await lock_a->lock();
  std::optional<decltype(held_a)> held_b;
  if (!same) held_b.emplace(co_await lock_b->lock());

  auto before_dir = co_await sample(dir.obj);
  Status precheck = Status::kOk;
  if (dir.exp->readonly) precheck = Status::kRofs;
  else if (!dir.exp->backend->caps().has(backend::Cap::kHardlink))
    precheck = Status::kNotsupp;
  else if (!valid_new_name(args->to.name)) precheck = Status::kAcces;
  if (precheck != Status::kOk) {
    auto file_attr = co_await sample(file->obj);
    begin_result(enc, ctx, call, precheck);
    encode_post_attr(enc, file_attr, file->exp->fsid);
    encode_wcc(enc, wcc_pre(before_dir), before_dir, dir.exp->fsid);
    co_await send_captured(ctx, enc, cap);
    co_return;
  }
  auto mapped = exports_.squash_cred(rpc_cred, *dir.exp);
  auto cred = mapped.view();
  auto linked = co_await dir.obj->link(cred, *file->obj, args->to.name);
  auto file_attr = co_await sample(file->obj);
  auto after_dir = co_await sample(dir.obj);
  begin_result(enc, ctx, call,
               linked ? Status::kOk : core::to_v3(linked.error(), Proc::kLink));
  encode_post_attr(enc, file_attr, file->exp->fsid);
  encode_wcc(enc, wcc_pre(before_dir), after_dir, dir.exp->fsid);
  co_await send_captured(ctx, enc, cap);
}

rt::Task<void> Engine::proc_commit(ConnCtx& ctx, RpcCall& call, const rpc::Cred& rpc_cred) {
  auto args = CommitArgs::decode(call.args);
  if (!args || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  auto resolved = co_await resolve(args->file, ctx.peer.addr);
  xdr::XdrEnc enc(ctx.pool);
  if (!resolved) {
    begin_result(enc, ctx, call, core::to_v3(resolved.error(), Proc::kCommit));
    encode_wcc(enc, std::nullopt, std::nullopt, 0);
    co_await send(ctx, enc);
    co_return;
  }
  auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
  auto held = co_await lock->lock_shared();  // flushing does not mutate
  auto before = co_await sample(resolved->obj);
  auto mapped = exports_.squash_cred(rpc_cred, *resolved->exp);
  auto cred = mapped.view();
  backend::OpenCtx open{cred, nullptr};
  auto committed = co_await resolved->obj->commit(open, args->offset, args->count);
  auto after = co_await sample(resolved->obj);
  if (!committed) {
    begin_result(enc, ctx, call, core::to_v3(committed.error(), Proc::kCommit));
    encode_wcc(enc, wcc_pre(before), after, resolved->exp->fsid);
  } else {
    begin_result(enc, ctx, call, Status::kOk);
    encode_wcc(enc, wcc_pre(before), after, resolved->exp->fsid);
    enc.opaque_fixed(verf_);
  }
  co_await send(ctx, enc);
}

}  // namespace lnfs::nfsv3
