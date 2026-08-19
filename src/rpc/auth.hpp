#pragma once
// RPC authentication (design 03 §3.5). AUTH_NONE and AUTH_SYS for v1; the Authenticator
// registry is the future slot for RPCSEC_GSS / TLS channel attributes.
//
// Note (phase 1): squash mapping happens here too — authenticate() will take the ExportEntry
// once the export table exists, so engines and backends only ever see the mapped Cred.

#include <memory>

#include "rpc/rpc_msg.hpp"
#include "util/result.hpp"
#include "util/small_vec.hpp"

namespace lnfs::rpc {

enum class AuthFlavor : uint32_t { kNone = 0, kSys = 1 };

struct Cred {
  uint32_t uid = 65534;  // nobody
  uint32_t gid = 65534;
  SmallVec<uint32_t, 16> gids;
  AuthFlavor flavor = AuthFlavor::kNone;
};

class Authenticator {
 public:
  virtual ~Authenticator() = default;
  // Err(EACCES) -> MSG_DENIED(AUTH_ERROR, AUTH_BADCRED)
  virtual Result<Cred> authenticate(const OpaqueAuth& cred, const OpaqueAuth& verf) = 0;
};

class AuthRegistry {
 public:
  void add(uint32_t flavor, std::unique_ptr<Authenticator> a);
  // Err(EPERM): unknown flavor (AUTH_REJECTEDCRED); Err(EACCES): bad cred body.
  Result<Cred> authenticate(const RpcCall& call) const;

  // AUTH_NONE + AUTH_SYS preinstalled.
  static AuthRegistry& default_registry();

 private:
  static constexpr size_t kMaxFlavor = 8;
  std::unique_ptr<Authenticator> by_flavor_[kMaxFlavor];
};

}  // namespace lnfs::rpc
