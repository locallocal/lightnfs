#pragma once
// RPC program dispatch (design 03 §3.4). Engines register {prog, vers range, handler};
// handle_request enforces the layered error discipline and auth, then hands the call to the
// engine, which decodes args and sends its own reply through ConnCtx.

#include <functional>
#include <vector>

#include "rpc/auth.hpp"
#include "rpc/rpc_msg.hpp"
#include "runtime/task.hpp"

namespace lnfs::transport {
struct ConnCtx;
}

namespace lnfs::rpc {

class Dispatcher {
 public:
  // The handler owns arg decoding and reply sending. Throwing out of it maps to SYSTEM_ERR.
  using Handler =
      std::function<rt::Task<void>(transport::ConnCtx&, RpcCall&, const Cred&)>;

  struct Program {
    uint32_t prog;
    uint32_t vers_lo, vers_hi;
    Handler handler;
  };

  explicit Dispatcher(AuthRegistry& auth = AuthRegistry::default_registry()) : auth_(auth) {}
  void add(Program p) { programs_.push_back(std::move(p)); }

  rt::Task<void> handle_request(transport::ConnCtx& ctx, rt::BufferChain rec);

  // Helper for engines: GARBAGE_ARGS on arg decode failure.
  static rt::Task<void> reply_garbage_args(transport::ConnCtx& ctx, uint32_t xid);

 private:
  AuthRegistry& auth_;
  std::vector<Program> programs_;
};

}  // namespace lnfs::rpc
