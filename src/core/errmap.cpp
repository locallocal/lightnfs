#include "core/errmap.hpp"

#include <array>
#include <cerrno>

namespace lnfs::core {
namespace {

using S = nfsv3::Status;
using P = nfsv3::Proc;

S raw_mapping(Errno error) {
  if (error == Errno::kBadHandle) return S::kBadhandle;
  if (error == Errno::kJukebox) return S::kJukebox;
  switch (raw(error)) {
    case EPERM: return S::kPerm;
    case ENOENT: return S::kNoent;
    case EIO: return S::kIo;
    case ENXIO: return S::kNxio;
    case EACCES: return S::kAcces;
    case EEXIST: return S::kExist;
    case EXDEV: return S::kXdev;
    case ENODEV: return S::kNodev;
    case ENOTDIR: return S::kNotdir;
    case EISDIR: return S::kIsdir;
    case EINVAL: return S::kInval;
    case EFBIG: return S::kFbig;
    case ENOSPC: return S::kNospc;
    case EROFS: return S::kRofs;
    case EMLINK: return S::kMlink;
    case ENAMETOOLONG: return S::kNametoolong;
    case ENOTEMPTY: return S::kNotempty;
#ifdef EDQUOT
    case EDQUOT: return S::kDquot;
#endif
    case ESTALE: return S::kStale;
    case EOPNOTSUPP: return S::kNotsupp;
    default: return S::kIo;
  }
}

template <size_t N>
bool one_of(S value, const std::array<S, N>& values) {
  for (S candidate : values)
    if (candidate == value) return true;
  return false;
}

}  // namespace

bool v3_error_allowed(P proc, S status) {
  if (status == S::kOk || status == S::kIo || status == S::kServerfault) return true;
  if (status == S::kStale || status == S::kBadhandle) return proc != P::kNull;
  switch (proc) {
    case P::kGetattr: return false;
    case P::kLookup:
      return one_of(status, std::array{S::kNoent, S::kAcces, S::kNotdir, S::kNametoolong});
    case P::kAccess: return false;
    case P::kReadlink:
      return one_of(status, std::array{S::kInval, S::kAcces, S::kNotsupp});
    case P::kRead:
      return one_of(status,
                    std::array{S::kNxio, S::kAcces, S::kIsdir, S::kInval, S::kJukebox});
    case P::kReaddir:
      return one_of(status,
                    std::array{S::kAcces, S::kNotdir, S::kBadCookie, S::kToosmall});
    case P::kReaddirplus:
      return one_of(status, std::array{S::kAcces, S::kNotdir, S::kBadCookie,
                                       S::kToosmall, S::kNotsupp});
    case P::kFsstat:
    case P::kFsinfo:
    case P::kPathconf: return false;
    default: return true;  // Phase-2 procedures have their complete mapping added with them.
  }
}

S to_v3(Errno error, P proc) {
  S mapped = raw_mapping(error);
  return v3_error_allowed(proc, mapped) ? mapped : S::kIo;
}

}  // namespace lnfs::core
