#include "nfsv3/engine.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>

#include "core/errmap.hpp"
#include "core/readdir.hpp"
#include "transport/connection.hpp"

namespace lnfs::nfsv3 {
namespace {

using rpc::RpcCall;
using transport::ConnCtx;

rt::Task<void> send(ConnCtx& ctx, xdr::XdrEnc& enc) { co_await ctx.send(enc.take()); }

void begin_result(xdr::XdrEnc& enc, uint32_t xid, Status status) {
  rpc::encode_reply_success(enc, xid);
  enc.u32(static_cast<uint32_t>(status));
}

bool valid_component(std::string_view name) {
  return !name.empty() && name.size() <= kMaxName &&
         name.find('/') == std::string_view::npos &&
         name.find('\0') == std::string_view::npos;
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
  // M1 is deliberately read-only. RPC-level PROC_UNAVAIL is structurally valid for every
  // omitted mutation and lets clients distinguish an unavailable milestone operation.
  if (!(proc == Proc::kGetattr || proc == Proc::kLookup || proc == Proc::kAccess ||
        proc == Proc::kReadlink || proc == Proc::kRead || proc == Proc::kReaddir ||
        proc == Proc::kReaddirplus || proc == Proc::kFsstat || proc == Proc::kFsinfo ||
        proc == Proc::kPathconf)) {
    xdr::XdrEnc enc(ctx.pool);
    rpc::encode_reply_accepted_err(enc, call.xid, rpc::kProcUnavail);
    co_await send(ctx, enc);
    co_return;
  }

  if (proc == Proc::kGetattr) {
    auto args = FileHandle::decode(call.args);
    if (!args || !call.args.at_end()) {
      co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
      co_return;
    }
    auto resolved = co_await resolve(*args, ctx.peer.addr);
    xdr::XdrEnc enc(ctx.pool);
    if (!resolved) {
      begin_result(enc, call.xid, core::to_v3(resolved.error(), proc));
    } else {
      auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
      auto held = co_await lock->lock_shared();
      auto attr = co_await resolved->obj->getattr();
      if (!attr) begin_result(enc, call.xid, core::to_v3(attr.error(), proc));
      else {
        begin_result(enc, call.xid, Status::kOk);
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
      begin_result(enc, call.xid, core::to_v3(dir.error(), proc));
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
      begin_result(enc, call.xid, core::to_v3(found.error(), proc));
      encode_post_attr(enc, attr_value(dir_attr), dir->exp->fsid);
    } else {
      auto obj_attr = co_await (*found)->getattr();
      FileHandle wire{handles_.encode(*dir->exp, (*found)->id())};
      begin_result(enc, call.xid, Status::kOk);
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
      begin_result(enc, call.xid, core::to_v3(resolved.error(), proc));
      encode_post_attr(enc, std::nullopt, 0);
    } else {
      auto mapped = exports_.squash_cred(rpc_cred, *resolved->exp);
      auto cred = mapped.view();
      auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
      auto held = co_await lock->lock_shared();
      auto allowed = co_await resolved->obj->access(cred, access_from_wire(args->access));
      auto attr = co_await resolved->obj->getattr();
      if (!allowed) {
        begin_result(enc, call.xid, core::to_v3(allowed.error(), proc));
        encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
      } else {
        if (resolved->exp->readonly)
          allowed->clear(backend::Access::kModify)
              .clear(backend::Access::kExtend)
              .clear(backend::Access::kDelete);
        begin_result(enc, call.xid, Status::kOk);
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
      begin_result(enc, call.xid, core::to_v3(resolved.error(), proc));
      encode_post_attr(enc, std::nullopt, 0);
    } else {
      auto lock = locks_.get(resolved->exp->fsid, resolved->oid);
      auto held = co_await lock->lock_shared();
      auto target = co_await resolved->obj->readlink();
      auto attr = co_await resolved->obj->getattr();
      if (!target) {
        begin_result(enc, call.xid, core::to_v3(target.error(), proc));
        encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
      } else {
        begin_result(enc, call.xid, Status::kOk);
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
      begin_result(enc, call.xid, core::to_v3(resolved.error(), proc));
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
      begin_result(enc, call.xid, core::to_v3(read.error(), proc));
      encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
    } else {
      begin_result(enc, call.xid, Status::kOk);
      encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
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
      begin_result(enc, call.xid, core::to_v3(resolved.error(), proc));
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
      begin_result(enc, call.xid, Status::kBadCookie);
      encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
      co_await send(ctx, enc);
      co_return;
    }
    constexpr size_t kFixedReply = 24 + 4 + 88 + 8 + 8;
    if (maxcount < kFixedReply || dircount < 24) {
      begin_result(enc, call.xid, Status::kToosmall);
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
      begin_result(enc, call.xid, status);
      encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
      co_await send(ctx, enc);
      co_return;
    }
    begin_result(enc, call.xid, Status::kOk);
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
    begin_result(enc, call.xid, core::to_v3(resolved.error(), proc));
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
      begin_result(enc, call.xid, core::to_v3(stats.error(), proc));
      encode_post_attr(enc, attr_value(attr), resolved->exp->fsid);
    } else {
      begin_result(enc, call.xid, Status::kOk);
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
    begin_result(enc, call.xid, Status::kOk);
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
    begin_result(enc, call.xid, Status::kOk);
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

}  // namespace lnfs::nfsv3
