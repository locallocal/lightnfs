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
    // nobody
    uint32_t uid = 65534;
    uint32_t gid = 65534;
    SmallVec<uint32_t, 16> gids;
    AuthFlavor flavor = AuthFlavor::kNone;
    // The caller claimed no identity at all (AUTH_NONE), so the export's anonymous
    // identity applies whatever its squash mode is — `none` is about passing a *claimed*
    // identity through, and there is none here (followups/protocol-conformance-followups.md C3).
    //
    // An explicit bit rather than `flavor == kNone`: flavor defaults to kNone, so every
    // default-constructed Cred — the test fixtures, any future internal caller — would
    // otherwise be silently anonymous.  Only the authenticator sets this.
    bool anonymous = false;
    // AUTH_SYS machinename (v4 principal comparisons)
    std::string machine;

    // v4 principal identity (RFC 8881 CLID_IN_USE checks): flavor + machine + uid.
    std::string principal() const {
        return std::to_string(static_cast<uint32_t>(flavor)) + "/" + machine + "/" + std::to_string(uid);
    }
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
