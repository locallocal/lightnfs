#pragma once

#include "core/config.hpp"
#include "core/file_handle.hpp"
#include "rpc/dispatch.hpp"

namespace lnfs::mountd {

inline constexpr uint32_t kProgram = 100005;
inline constexpr uint32_t kVersion = 3;

class Mount3 {
 public:
  Mount3(core::ExportTable& exports, core::FileHandleCodec& handles)
      : exports_(exports), handles_(handles) {}

  void register_with(rpc::Dispatcher& dispatcher);
  rt::Task<void> dispatch(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&);

 private:
  core::ExportTable& exports_;
  core::FileHandleCodec& handles_;
};

}  // namespace lnfs::mountd
