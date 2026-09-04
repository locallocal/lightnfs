#pragma once
// In-process stand-in for libcephfs (plan doc 10 §5.3): serves the backend/cephapi.hpp
// function table over a local directory so tests/test_cephfs.cpp exercises the whole
// CephFS backend — handle codec, inode/Fh caches, identity plumbing, readdir cookies,
// IO/commit, v4.2 ops, native change counter, native locks, jukebox mapping — under
// ctest without a cluster.  Semantics follow the real library where the backend
// depends on them: every call returns a negative errno; UserPerm carries the caller
// identity; inode numbers are never reused (a re-created file gets a fresh one, so
// the old handle is ENOENT → ESTALE, P2); stx_dev carries the snapid and stx_version
// a change counter bumped on every mutation; fcntl locks are keyed by (inode, owner)
// and dropped when the Fh that took them is closed.

#include <cstdint>
#include <memory>
#include <string>

#include "backend/cephapi.hpp"

namespace lnfs::testing {

struct FakeCephApi {
  // The table every fake-backed CephBackend uses.
  static std::shared_ptr<const backend::cephapi::Api> api();

  // The directory that stands in for the filesystem root (ceph_mount binds to it).
  static void set_root(std::string dir);
  // ceph_mount fails with -err when non-zero.
  static void fail_mount(int err);
  // The next `count` calls (metadata or data) fail with -err — transport-error tests.
  static void fail_next(int err, int count = 1);
  // Identity the last call ran under (from its UserPerm).
  static uint32_t last_uid();
  static uint32_t last_gid();
  // Leak assertions: live Inode / Fh / dir handle / UserPerm counts.
  static int live_inodes();
  static int live_fhs();
  static int live_dirs();
  static int live_perms();
  // Number of ceph_ll_getattr calls (round-trip accounting).
  static uint64_t getattr_calls();
};

}  // namespace lnfs::testing
