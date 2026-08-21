#pragma once

#include "nfsv3/nfs3_types.hpp"
#include "nfsv4/nfs4_types.hpp"
#include "util/errno.hpp"

namespace lnfs::core {

nfsv3::Status to_v3(Errno error, nfsv3::Proc proc);
bool v3_error_allowed(nfsv3::Proc proc, nfsv3::Status status);

// v4 whitelist mapping (RFC 8881 §15.2, per implemented op; design 04 §4.6):
// out-of-table results degrade to IO/SERVERFAULT rather than leak.
nfsv4::Status to_v4(Errno error, nfsv4::Op op);
bool v4_error_allowed(nfsv4::Op op, nfsv4::Status status);

}  // namespace lnfs::core
