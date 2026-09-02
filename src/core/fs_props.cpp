#include "core/fs_props.hpp"

#include <algorithm>

namespace lnfs::core {

FsProps fs_props(const backend::Backend& backend) {
  FsProps out;
  out.limits = backend.limits();
  out.limits.pref_read = std::min(out.limits.pref_read, out.limits.max_read);
  out.limits.pref_write = std::min(out.limits.pref_write, out.limits.max_write);
  auto caps = backend.caps();
  out.link_support = caps.has(backend::Cap::kHardlink);
  out.symlink_support = caps.has(backend::Cap::kSymlink);
  out.case_insensitive = caps.has(backend::Cap::kCaseInsensitive);
  out.native_change = caps.has(backend::Cap::kNativeChange);
  out.native_access = caps.has(backend::Cap::kNativeAccess);
  return out;
}

}  // namespace lnfs::core
