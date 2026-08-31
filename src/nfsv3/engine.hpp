#pragma once

#include "core/boot_epoch.hpp"
#include "core/config.hpp"
#include "core/file_handle.hpp"
#include "core/obj_lock.hpp"
#include "nfsv3/nfs3_types.hpp"
#include "rpc/dispatch.hpp"

namespace lnfs::rpc {
class Drc;
}

namespace lnfs::nfsv3 {

class Engine {
 public:
  Engine(core::ExportTable& exports, core::FileHandleCodec& handles,
         core::ObjLockRegistry& locks)
      : exports_(exports), handles_(handles), locks_(locks) {}

  // v3 write/commit verifier = persisted boot epoch (design 04 §4.2).
  void set_write_verifier(core::WriteVerf verf) { verf_ = verf; }
  // Duplicate request cache for the non-idempotent procedures (design 03 §3.7).
  void set_drc(rpc::Drc* drc) { drc_ = drc; }

  void register_with(rpc::Dispatcher& dispatcher);
  rt::Task<void> dispatch(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&);

 private:
  struct Resolved {
    core::ExportEntry* exp;
    backend::ObjPtr obj;
    backend::ObjId oid;
  };
  // Reply bytes captured for the DRC; null for procedures it does not cache.
  using Capture = std::vector<std::byte>;
  // Every procedure handler has this shape so dispatch_proc is a jump table
  // (plan doc 10 §6.6).
  using Handler = rt::Task<void> (Engine::*)(transport::ConnCtx&, rpc::RpcCall&,
                                             const rpc::Cred&, Capture*);

  rt::Task<Result<Resolved>> resolve(const FileHandle&, const sockaddr_storage&);
  rt::Task<void> dispatch_proc(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&,
                               Capture* cap);

  // Read-only procedures (shared object lock).
  rt::Task<void> proc_getattr(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_lookup(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_access(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_readlink(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_read(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_readdir(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_fs_query(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_commit(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  // Mutating procedures (core::MutateGuard sequence, plan doc 10 §6.1).
  rt::Task<void> proc_setattr(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_write(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_create(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_mkdir(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_symlink(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_mknod(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_remove(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_rmdir(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_rename(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);
  rt::Task<void> proc_link(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&, Capture*);

  core::ExportTable& exports_;
  core::FileHandleCodec& handles_;
  core::ObjLockRegistry& locks_;
  core::WriteVerf verf_{};
  rpc::Drc* drc_ = nullptr;
};

}  // namespace lnfs::nfsv3
