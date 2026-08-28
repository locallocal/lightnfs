#include "mountd/mount3.hpp"

#include "obs/metrics.hpp"

#include <cerrno>

#include "transport/connection.hpp"

namespace lnfs::mountd {
namespace {

enum class MountStatus : uint32_t {
  kOk = 0,
  kPerm = 1,
  kNoent = 2,
  kIo = 5,
  kAcces = 13,
  kNotdir = 20,
  kInval = 22,
  kNametoolong = 63,
  kNotsupp = 10004,
  kServerfault = 10006,
};

MountStatus map_error(Errno error) {
  switch (raw(error)) {
    case EPERM: return MountStatus::kPerm;
    case ENOENT:
    case ESTALE: return MountStatus::kNoent;
    case EACCES: return MountStatus::kAcces;
    case ENOTDIR: return MountStatus::kNotdir;
    case EINVAL: return MountStatus::kInval;
    case ENAMETOOLONG: return MountStatus::kNametoolong;
    case EOPNOTSUPP: return MountStatus::kNotsupp;
    default: return MountStatus::kIo;
  }
}

rt::Task<void> send(transport::ConnCtx& ctx, xdr::XdrEnc& enc) {
  co_await ctx.send(enc.take());
}

bool valid_path_component(std::string_view part) {
  return !part.empty() && part != "." && part != ".." && part.size() <= 255 &&
         part.find('/') == std::string_view::npos && part.find('\0') == std::string_view::npos;
}

}  // namespace

void Mount3::register_with(rpc::Dispatcher& dispatcher) {
  dispatcher.add(
      {kProgram, kVersion, kVersion, this,
       [](void* self, transport::ConnCtx& ctx, rpc::RpcCall& call, const rpc::Cred& cred) {
         return static_cast<Mount3*>(self)->dispatch(ctx, call, cred);
       }});
}

rt::Task<void> Mount3::dispatch(transport::ConnCtx& ctx, rpc::RpcCall& call,
                                const rpc::Cred& rpc_cred) {
  obs::Metrics::instance().mount_calls.fetch_add(1, std::memory_order_relaxed);
  if (call.proc > 5) {
    xdr::XdrEnc enc(ctx.pool);
    rpc::encode_reply_accepted_err(enc, call.xid, rpc::kProcUnavail);
    co_await send(ctx, enc);
    co_return;
  }
  if (call.proc == 0 || call.proc == 2 || call.proc == 4) {
    xdr::XdrEnc enc(ctx.pool);
    rpc::encode_reply_success(enc, call.xid);
    if (call.proc == 2) enc.boolean(false);  // empty mountlist
    co_await send(ctx, enc);
    co_return;
  }
  if (call.proc == 3) {  // UMNT: consume path but keep no authoritative rmtab.
    auto path = call.args.string(1024);
    if (!path || !call.args.at_end()) {
      co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
      co_return;
    }
    xdr::XdrEnc enc(ctx.pool);
    rpc::encode_reply_success(enc, call.xid);
    co_await send(ctx, enc);
    co_return;
  }
  if (call.proc == 5) {  // EXPORT
    if (!call.args.at_end()) {
      co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
      co_return;
    }
    xdr::XdrEnc enc(ctx.pool);
    rpc::encode_reply_success(enc, call.xid);
    for (const auto& exp : exports_.entries()) {
      enc.boolean(true);
      enc.string(exp->path);
      for (const auto& client : exp->client_list()) {
        enc.boolean(true);
        enc.string(client.text());
      }
      enc.boolean(false);  // end groups
    }
    enc.boolean(false);  // end exports
    co_await send(ctx, enc);
    co_return;
  }

  auto path_arg = call.args.string(1024);
  if (!path_arg || !call.args.at_end()) {
    co_await rpc::Dispatcher::reply_garbage_args(ctx, call.xid);
    co_return;
  }
  std::string relative;
  core::ExportEntry* exp = exports_.for_mount_path(*path_arg, relative);
  xdr::XdrEnc enc(ctx.pool);
  rpc::encode_reply_success(enc, call.xid);
  if (!exp || !exports_.check_client(ctx.peer.addr, *exp)) {
    enc.u32(static_cast<uint32_t>(MountStatus::kAcces));
    co_await send(ctx, enc);
    co_return;
  }
  auto mapped = exports_.squash_cred(rpc_cred, *exp);
  auto cred = mapped.view();
  auto obj = co_await exp->backend->root();
  Errno failure = Errno::kOk;
  while (obj && !relative.empty()) {
    size_t slash = relative.find('/');
    std::string part = relative.substr(0, slash);
    if (!valid_path_component(part)) {
      failure = errno_from(EINVAL);
      break;
    }
    auto next = co_await (*obj)->lookup(cred, part);
    if (!next) {
      failure = next.error();
      break;
    }
    obj = std::move(next);
    if (slash == std::string::npos) relative.clear();
    else relative.erase(0, slash + 1);
  }
  if (!obj || failure != Errno::kOk) {
    enc.u32(static_cast<uint32_t>(map_error(obj ? failure : obj.error())));
    co_await send(ctx, enc);
    co_return;
  }
  auto fh = handles_.encode(*exp, (*obj)->id());
  enc.u32(static_cast<uint32_t>(MountStatus::kOk));
  enc.opaque(fh);
  enc.u32(1);  // auth_flavors length
  enc.u32(static_cast<uint32_t>(rpc::AuthFlavor::kSys));
  co_await send(ctx, enc);
}

}  // namespace lnfs::mountd
