#pragma once

#include "core/config.hpp"
#include "core/file_handle.hpp"
#include "rpc/dispatch.hpp"

namespace lnfs::mountd {

inline constexpr uint32_t kProgram = 100005;
inline constexpr uint32_t kVersion = 3;
// dirpath is string<MNTPATHLEN> with MNTPATHLEN = 1024 (RFC 1813 §5.1.1).  Decoding
// stops well past it so an over-long path answers MNT3ERR_NAMETOOLONG — the status the
// protocol has for exactly this — instead of a bare GARBAGE_ARGS the client can only
// read as EIO (followups/protocol-conformance-followups.md A3).
inline constexpr uint32_t kMaxMountPath = 1024;
inline constexpr uint32_t kMountPathWireMax = 8192;

class Mount3 {
 public:
    Mount3(core::ExportTable& exports, core::FileHandleCodec& handles) : exports_(exports), handles_(handles) {}

    void register_with(rpc::Dispatcher& dispatcher);
    rt::Task<void> dispatch(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&);

 private:
    core::ExportTable& exports_;
    core::FileHandleCodec& handles_;
};

}  // namespace lnfs::mountd
