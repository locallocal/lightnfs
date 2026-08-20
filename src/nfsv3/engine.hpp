#pragma once

#include "core/config.hpp"
#include "core/file_handle.hpp"
#include "core/obj_lock.hpp"
#include "nfsv3/nfs3_types.hpp"
#include "rpc/dispatch.hpp"

namespace lnfs::nfsv3 {

class Engine {
 public:
  Engine(core::ExportTable& exports, core::FileHandleCodec& handles,
         core::ObjLockRegistry& locks)
      : exports_(exports), handles_(handles), locks_(locks) {}

  void register_with(rpc::Dispatcher& dispatcher);
  rt::Task<void> dispatch(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&);

 private:
  struct Resolved {
    core::ExportEntry* exp;
    backend::ObjPtr obj;
    backend::ObjId oid;
  };

  rt::Task<Result<Resolved>> resolve(const FileHandle&, const sockaddr_storage&);

  core::ExportTable& exports_;
  core::FileHandleCodec& handles_;
  core::ObjLockRegistry& locks_;
};

}  // namespace lnfs::nfsv3
