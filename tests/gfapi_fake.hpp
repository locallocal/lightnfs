#pragma once
// In-process stand-in for libgfapi (plan doc 10 §5.3): serves the backend/gluster/gfapi.hpp
// function table over a local directory so tests/test_gluster.cpp exercises the
// whole GlusterFS backend — handle codec, object/glfd caches, identity plumbing,
// readdir cookies, IO/commit, v4.2 ops, native locks, jukebox mapping — under ctest
// without a cluster.  Semantics follow the real library where the backend depends on
// them: glfs_setfs* are thread-local; handles are 16 bytes and survive re-creation
// only as a different value (P2); xreaddirplus objects are owned by the xstat;
// posix locks are keyed by (inode, lk-owner) so two owners in one process conflict.

#include <cstdint>
#include <memory>
#include <string>

#include "backend/gluster/gfapi.hpp"

namespace lnfs::testing {

struct FakeGfapi {
  // The table every fake-backed GlusterBackend uses.
  static std::shared_ptr<const backend::gfapi::Api> api();

  // The directory that stands in for the volume (glfs_new/glfs_init bind to it).
  static void set_root(std::string dir);
  // glfs_init fails with `err` when non-zero.
  static void fail_init(int err);
  // The next `count` fops (metadata or data) fail with `err` — transport-error tests.
  static void fail_next(int err, int count = 1);
  // Identity the last fop ran under (thread-local in the real library too).
  static uint32_t last_fsuid();
  static uint32_t last_fsgid();
  // Leak assertions: live glfs_object / glfs_fd counts.
  static int live_objects();
  static int live_fds();
  // Number of glfs_h_access calls (round-trip accounting).
  static uint64_t access_calls();

  // A failed gateway's residue (plan 10 E2): an exclusive lock on `rel_path` under a
  // ghost owner; false if the file is missing.  release_stale_locks_after(ms) drops it
  // from a timer thread — the brick letting go on ping-timeout.
  static bool plant_stale_lock(const std::string& rel_path, uint64_t start, uint64_t len);
  static size_t stale_locks();
  static void release_stale_locks_after(int ms);
  static void join_stale_timer();  // join the pending release (before process exit)
};

}  // namespace lnfs::testing
