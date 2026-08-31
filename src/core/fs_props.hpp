#pragma once
// Filesystem-level properties derived once from a backend's capability bits and limits
// (plan doc 10 §6.4).  v3 FSINFO/PATHCONF and the v4 per-fs attributes (link_support,
// symlink_support, case_insensitive, max*, time_delta, ...) all read this struct instead
// of each re-deriving it from caps()/limits().  Space/inode counters stay in
// backend::FsStats, which both protocols already encode field-for-field.

#include "backend/api.hpp"

namespace lnfs::core {

struct FsProps {
  backend::FsLimits limits;  // pref_* clamped to max_*
  bool link_support = false;
  bool symlink_support = false;
  bool case_insensitive = false;
  // Fixed for every backend this server fronts (design 05): one root, names kept as
  // given, long names rejected rather than truncated, chown needs privilege, times are
  // settable.
  static constexpr bool kHomogeneous = true;
  static constexpr bool kCasePreserving = true;
  static constexpr bool kNoTrunc = true;
  static constexpr bool kChownRestricted = true;
  static constexpr bool kCansettime = true;
};

// From a real backend.  A default-constructed FsProps is the pseudo-fs answer
// (default limits, no link/symlink support).
FsProps fs_props(const backend::Backend& backend);

}  // namespace lnfs::core
