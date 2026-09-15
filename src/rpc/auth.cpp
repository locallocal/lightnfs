#include "rpc/auth.hpp"

namespace lnfs::rpc {

namespace {

class AuthNone final : public Authenticator {
 public:
    Result<Cred> authenticate(const OpaqueAuth&, const OpaqueAuth&) override {
        Cred out;
        // No identity claimed: squash_cred() maps this onto the export's anon_uid/anon_gid.
        // The 65534 defaults stay as the fallback for a path that never squashes.
        out.anonymous = true;
        return out;
    }
};

// AUTH_SYS body (RFC 5531 appendix A): stamp, machinename<255>, uid, gid, gids<16>.
class AuthSys final : public Authenticator {
 public:
    Result<Cred> authenticate(const OpaqueAuth& cred, const OpaqueAuth& verf) override {
        // RFC 5531 §8.2: an AUTH_SYS credential travels with an AUTH_NONE verifier of
        // zero length.  Anything else is AUTH_BADVERF rather than something to ignore
        // (followups/protocol-gaps.md C3) — no client sends anything else, and silently
        // accepting a verifier we never look at is how a flavor confusion goes unnoticed.
        if (verf.flavor != static_cast<uint32_t>(AuthFlavor::kNone) || !verf.body.empty()) return Err(Errno::kBadVerf);
        // flat mode; body references the request record
        xdr::XdrDec dec(cred.body);

        Cred out;
        out.flavor = AuthFlavor::kSys;
        // stamp
        if (!dec.u32()) return Err(errno_from(EACCES));
        auto name = dec.string(255);
        if (!name) return Err(errno_from(EACCES));
        out.machine = std::string(*name);
        auto uid = dec.u32();
        auto gid = dec.u32();
        auto ngids = dec.u32();
        if (!uid || !gid || !ngids || *ngids > 16) return Err(errno_from(EACCES));
        out.uid = *uid;
        out.gid = *gid;
        for (uint32_t i = 0; i < *ngids; ++i) {
            auto g = dec.u32();
            if (!g) return Err(errno_from(EACCES));
            out.gids.push_back(*g);
        }
        return out;
    }
};

}  // namespace

void AuthRegistry::add(uint32_t flavor, std::unique_ptr<Authenticator> a) {
    if (flavor < kMaxFlavor) by_flavor_[flavor] = std::move(a);
}

Result<Cred> AuthRegistry::authenticate(const RpcCall& call) const {
    uint32_t f = call.cred.flavor;
    if (f >= kMaxFlavor || !by_flavor_[f]) return Err(errno_from(EPERM));
    return by_flavor_[f]->authenticate(call.cred, call.verf);
}

AuthRegistry& AuthRegistry::default_registry() {
    static AuthRegistry reg = [] {
        AuthRegistry r;
        r.add(0, std::make_unique<AuthNone>());
        r.add(1, std::make_unique<AuthSys>());
        return r;
    }();
    return reg;
}

}  // namespace lnfs::rpc
