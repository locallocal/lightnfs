#pragma once

#include "nfsv3/nfs3_types.hpp"
#include "util/errno.hpp"

namespace lnfs::core {

nfsv3::Status to_v3(Errno error, nfsv3::Proc proc);
bool v3_error_allowed(nfsv3::Proc proc, nfsv3::Status status);

}  // namespace lnfs::core
