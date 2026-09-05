#pragma once
// libcephfs binding for the CephFS backend (design 06 §6.8, plan doc 10 §5.3 — the
// fourth backend, and the first one with both halves of multi-gateway coherence:
// a native change counter and native byte-range locks).
//
// The backend never calls libcephfs symbols directly: it goes through this function
// table, filled either by dlopen("libcephfs.so.2") at backend start (no build-time
// Ceph dependency, the build matrix does not grow) or by the tests' in-process fake
// (tests/cephapi_fake.cpp), which serves the same table over a local directory so the
// whole backend logic runs under ctest without a cluster.
//
// Signatures are the Ceph 20 (Tentacle) ones from cephfs/libcephfs.h and
// cephfs/ceph_ll_client.h; the opaque types are declared in the global namespace
// under their real names so a translation unit that also includes the real headers
// can static_assert that every member matches (scripts/check_cephapi_abi.sh).  The
// two structs the backend needs complete (ceph_statx, vinodeno_t) are defined below
// under the real header's include guard: when the real header is present its
// definitions win and the pointer types still compare equal.

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <string>

#include "util/result.hpp"

struct ceph_mount_info;
struct ceph_dir_result;
struct Inode;
class Fh;  // libcephfs.h declares it `class Fh` under C++
struct UserPerm;

#ifndef CEPH_CEPH_LL_CLIENT_H
// C layout of cephfs/ceph_ll_client.h's vinodeno_t: {inodeno_t ino; snapid_t snapid}
// — two uint64_t, passed by value (ceph_ll_lookup_vino).  The C++ side of the real
// header only forward-declares it, so the backend carries its own complete type.
struct vinodeno_t {
  uint64_t ino;
  uint64_t snapid;
};
struct ceph_statx {
  uint32_t stx_mask;
  uint32_t stx_blksize;
  uint32_t stx_nlink;
  uint32_t stx_uid;
  uint32_t stx_gid;
  uint16_t stx_mode;
  uint64_t stx_ino;
  uint64_t stx_size;
  uint64_t stx_blocks;
  dev_t stx_dev;   // libcephfs fills this with the inode's snapid (Ganesha relies on it)
  dev_t stx_rdev;
  struct timespec stx_atime;
  struct timespec stx_ctime;
  struct timespec stx_mtime;
  struct timespec stx_btime;
  uint64_t stx_version;  // the MDS change attribute → Attr::change (kNativeChange)
};
#endif

namespace lnfs::backend::cephapi {

// ceph_ll_client.h CEPH_STATX_*
inline constexpr unsigned kStatxMode = 0x1, kStatxNlink = 0x2, kStatxUid = 0x4,
                          kStatxGid = 0x8, kStatxRdev = 0x10, kStatxAtime = 0x20,
                          kStatxMtime = 0x40, kStatxCtime = 0x80, kStatxIno = 0x100,
                          kStatxSize = 0x200, kStatxBlocks = 0x400,
                          kStatxBasicStats = 0x7ff, kStatxBtime = 0x800,
                          kStatxVersion = 0x1000, kStatxAllStats = 0x1fff;
// ceph_ll_client.h CEPH_SETATTR_*
inline constexpr int kSetMode = 1 << 0, kSetUid = 1 << 1, kSetGid = 1 << 2,
                     kSetMtime = 1 << 3, kSetAtime = 1 << 4, kSetSize = 1 << 5,
                     kSetCtime = 1 << 6, kSetMtimeNow = 1 << 7, kSetAtimeNow = 1 << 8;
// ceph_ll_client.h AT_STATX_DONT_SYNC (attributes from the client cache, no MDS trip)
inline constexpr unsigned kStatxDontSync = 0x4000;
// libcephfs.h CEPH_NOSNAP: the head (non-snapshot) version of an inode
inline constexpr uint64_t kNoSnap = static_cast<uint64_t>(-2);
// libcephfs.h CEPH_RECLAIM_RESET: evict the session being reclaimed instead of
// inheriting its state (design 09 §9.7, plan 10 D2)
inline constexpr unsigned kReclaimReset = 1;
// ceph_ll_client.h FALLOC_FL_* (Ceph accepts mode 0 and PUNCH_HOLE|KEEP_SIZE only)
inline constexpr int kFallocKeepSize = 0x01, kFallocPunchHole = 0x02;

// Every entry returns 0 / a count on success and a negative errno on failure
// (libcephfs never uses the global errno).
struct Api {
  // lifecycle
  const char* (*ceph_version)(int* major, int* minor, int* patch) = nullptr;
  int (*ceph_create)(ceph_mount_info** cmount, const char* const id) = nullptr;
  int (*ceph_conf_read_file)(ceph_mount_info*, const char* path_list) = nullptr;
  int (*ceph_conf_set)(ceph_mount_info*, const char* option, const char* value) = nullptr;
  int (*ceph_conf_get)(ceph_mount_info*, const char* option, char* buf, size_t len) = nullptr;
  int (*ceph_init)(ceph_mount_info*) = nullptr;
  int (*ceph_select_filesystem)(ceph_mount_info*, const char* fs_name) = nullptr;
  int (*ceph_mount)(ceph_mount_info*, const char* root) = nullptr;
  int (*ceph_unmount)(ceph_mount_info*) = nullptr;
  int (*ceph_release)(ceph_mount_info*) = nullptr;
  void (*ceph_shutdown)(ceph_mount_info*) = nullptr;
  int64_t (*ceph_get_fs_cid)(ceph_mount_info*) = nullptr;
  uint64_t (*ceph_get_instance_id)(ceph_mount_info*) = nullptr;
  // caller identity
  UserPerm* (*ceph_userperm_new)(uid_t, gid_t, int ngids, gid_t* gidlist) = nullptr;
  void (*ceph_userperm_destroy)(UserPerm*) = nullptr;
  // inodes
  int (*ceph_ll_lookup_root)(ceph_mount_info*, Inode** parent) = nullptr;
  int (*ceph_ll_lookup_vino)(ceph_mount_info*, vinodeno_t vino, Inode** inode) = nullptr;
  int (*ceph_ll_lookup)(ceph_mount_info*, Inode* parent, const char* name, Inode** out,
                        struct ceph_statx* stx, unsigned want, unsigned flags,
                        const UserPerm* perms) = nullptr;
  int (*ceph_ll_put)(ceph_mount_info*, Inode* in) = nullptr;
  int (*ceph_ll_getattr)(ceph_mount_info*, Inode* in, struct ceph_statx* stx, unsigned want,
                         unsigned flags, const UserPerm* perms) = nullptr;
  int (*ceph_ll_setattr)(ceph_mount_info*, Inode* in, struct ceph_statx* stx, int mask,
                         const UserPerm* perms) = nullptr;
  int (*ceph_ll_open)(ceph_mount_info*, Inode* in, int flags, Fh** fh,
                      const UserPerm* perms) = nullptr;
  int (*ceph_ll_create)(ceph_mount_info*, Inode* parent, const char* name, mode_t mode,
                        int oflags, Inode** outp, Fh** fhp, struct ceph_statx* stx,
                        unsigned want, unsigned lflags, const UserPerm* perms) = nullptr;
  int (*ceph_ll_mknod)(ceph_mount_info*, Inode* parent, const char* name, mode_t mode,
                       dev_t rdev, Inode** out, struct ceph_statx* stx, unsigned want,
                       unsigned flags, const UserPerm* perms) = nullptr;
  int (*ceph_ll_mkdir)(ceph_mount_info*, Inode* parent, const char* name, mode_t mode,
                       Inode** out, struct ceph_statx* stx, unsigned want, unsigned flags,
                       const UserPerm* perms) = nullptr;
  int (*ceph_ll_symlink)(ceph_mount_info*, Inode* in, const char* name, const char* value,
                         Inode** out, struct ceph_statx* stx, unsigned want, unsigned flags,
                         const UserPerm* perms) = nullptr;
  int (*ceph_ll_link)(ceph_mount_info*, Inode* in, Inode* newparent, const char* name,
                      const UserPerm* perms) = nullptr;
  int (*ceph_ll_unlink)(ceph_mount_info*, Inode* in, const char* name,
                        const UserPerm* perms) = nullptr;
  int (*ceph_ll_rmdir)(ceph_mount_info*, Inode* in, const char* name,
                       const UserPerm* perms) = nullptr;
  int (*ceph_ll_rename)(ceph_mount_info*, Inode* parent, const char* name, Inode* newparent,
                        const char* newname, const UserPerm* perms) = nullptr;
  int (*ceph_ll_readlink)(ceph_mount_info*, Inode* in, char* buf, size_t bufsize,
                          const UserPerm* perms) = nullptr;
  int (*ceph_ll_statfs)(ceph_mount_info*, Inode* in, struct statvfs* stbuf) = nullptr;
  uint32_t (*ceph_ll_stripe_unit)(ceph_mount_info*, Inode* in) = nullptr;
  // file handles
  int (*ceph_ll_read)(ceph_mount_info*, Fh* filehandle, int64_t off, uint64_t len,
                      char* buf) = nullptr;
  int (*ceph_ll_write)(ceph_mount_info*, Fh* filehandle, int64_t off, uint64_t len,
                       const char* data) = nullptr;
  int64_t (*ceph_ll_writev)(ceph_mount_info*, Fh* fh, const struct iovec* iov, int iovcnt,
                            int64_t off) = nullptr;
  int (*ceph_ll_fsync)(ceph_mount_info*, Fh* fh, int syncdataonly) = nullptr;
  off_t (*ceph_ll_lseek)(ceph_mount_info*, Fh* filehandle, off_t offset, int whence) = nullptr;
  int (*ceph_ll_fallocate)(ceph_mount_info*, Fh* fh, int mode, int64_t offset,
                           int64_t length) = nullptr;
  int (*ceph_ll_close)(ceph_mount_info*, Fh* filehandle) = nullptr;
  int (*ceph_ll_getlk)(ceph_mount_info*, Fh* fh, struct flock* fl, uint64_t owner) = nullptr;
  int (*ceph_ll_setlk)(ceph_mount_info*, Fh* fh, struct flock* fl, uint64_t owner,
                       int sleep) = nullptr;
  // directories
  int (*ceph_ll_opendir)(ceph_mount_info*, Inode* in, ceph_dir_result** dirpp,
                         const UserPerm* perms) = nullptr;
  int (*ceph_ll_releasedir)(ceph_mount_info*, ceph_dir_result* dir) = nullptr;
  int (*ceph_readdirplus_r)(ceph_mount_info*, ceph_dir_result* dirp, struct dirent* de,
                            struct ceph_statx* stx, unsigned want, unsigned flags,
                            Inode** out) = nullptr;
  void (*ceph_seekdir)(ceph_mount_info*, ceph_dir_result* dirp, int64_t offset) = nullptr;
  // state reclaim (design 09 §9.7, plan 10 D2) — optional: a libcephfs without them
  // (pre-Nautilus) still loads; CephBackend::takeover() then answers ENOTSUP.
  // set_uuid/start_reclaim want an initialised, *unmounted* handle.
  void (*ceph_set_uuid)(ceph_mount_info*, const char* uuid) = nullptr;
  int (*ceph_start_reclaim)(ceph_mount_info*, const char* uuid, unsigned flags) = nullptr;
  void (*ceph_finish_reclaim)(ceph_mount_info*) = nullptr;
};

// dlopen(libcephfs.so.2) and resolve every entry of Api.  Errors: ENOENT when the
// library is not installed, ENOEXEC when it lacks one of the required symbols
// (older than Ceph 15 "Octopus", where ceph_ll_lookup_vino appeared).  The handle
// stays open for the life of the process.
Result<std::shared_ptr<const Api>> load_system_api(std::string* detail = nullptr);

// Every required pointer set?  (The fake and the loader both go through this before
// use; the optional reclaim entries may stay null.)
bool complete(const Api& api);
// The three reclaim entries present?
bool reclaim_supported(const Api& api);

}  // namespace lnfs::backend::cephapi
