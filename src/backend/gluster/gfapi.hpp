#pragma once
// libgfapi binding for the GlusterFS backend (design 06 §6.6, plan doc 10 §5.3).
//
// The backend never calls libgfapi symbols directly: it goes through this function
// table, which is filled either by dlopen("libgfapi.so.0") at backend start (no
// build-time dependency on GlusterFS, so the binary stays a single package and the
// build matrix does not grow) or by the tests' in-process fake (tests/gfapi_fake.cpp),
// which serves the same table over a local directory so the whole backend logic runs
// under ctest without a cluster.
//
// Signatures are the GlusterFS 11 ones (glfs.h / glfs-handles.h); the opaque structs
// are declared in the global namespace under their real names so a translation unit
// that also includes the real headers can static_assert the pointer types match
// (scripts/check_gfapi_abi.sh does exactly that when the headers are present).

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <string>

#include "util/result.hpp"

struct glfs;
struct glfs_fd;
struct glfs_object;
struct glfs_stat;  // also the name of a function in glfs.h: always `struct glfs_stat`
struct glfs_xreaddirp_stat;

namespace lnfs::backend::gfapi {

// glfs-handles.h
inline constexpr int kHandleLength = 16;  // GFAPI_HANDLE_LENGTH: a GFID (UUID)
// glfs.h GFAPI_SET_ATTR_*
inline constexpr int kSetMode = 0x1, kSetUid = 0x2, kSetGid = 0x4, kSetSize = 0x8,
                     kSetAtime = 0x10, kSetMtime = 0x20;
// glfs.h GFAPI_XREADDIRP_*
inline constexpr uint32_t kXreaddirpStat = 0x1, kXreaddirpHandle = 0x2;

struct Api {
  // lifecycle
  glfs* (*glfs_new)(const char* volname) = nullptr;
  int (*glfs_set_volfile_server)(glfs*, const char* transport, const char* host,
                                 int port) = nullptr;
  int (*glfs_set_logging)(glfs*, const char* logfile, int loglevel) = nullptr;
  int (*glfs_init)(glfs*) = nullptr;
  int (*glfs_fini)(glfs*) = nullptr;
  int (*glfs_get_volumeid)(glfs*, char* volid, size_t size) = nullptr;
  // per-thread caller identity
  int (*glfs_setfsuid)(uid_t) = nullptr;
  int (*glfs_setfsgid)(gid_t) = nullptr;
  int (*glfs_setfsgroups)(size_t, const gid_t*) = nullptr;
  // handle-based namespace ops
  glfs_object* (*glfs_h_lookupat)(glfs*, glfs_object* parent, const char* path,
                                  struct stat*, int follow) = nullptr;
  glfs_object* (*glfs_h_creat)(glfs*, glfs_object* parent, const char* path, int flags,
                               mode_t, struct stat*) = nullptr;
  glfs_object* (*glfs_h_mkdir)(glfs*, glfs_object* parent, const char* path, mode_t,
                               struct stat*) = nullptr;
  glfs_object* (*glfs_h_mknod)(glfs*, glfs_object* parent, const char* path, mode_t,
                               dev_t, struct stat*) = nullptr;
  glfs_object* (*glfs_h_symlink)(glfs*, glfs_object* parent, const char* name,
                                 const char* data, struct stat*) = nullptr;
  int (*glfs_h_unlink)(glfs*, glfs_object* parent, const char* path) = nullptr;
  int (*glfs_h_close)(glfs_object*) = nullptr;
  int (*glfs_h_truncate)(glfs*, glfs_object*, off_t) = nullptr;
  int (*glfs_h_stat)(glfs*, glfs_object*, struct stat*) = nullptr;
  int (*glfs_h_statfs)(glfs*, glfs_object*, struct statvfs*) = nullptr;
  int (*glfs_h_setattrs)(glfs*, glfs_object*, struct stat*, int valid) = nullptr;
  int (*glfs_h_readlink)(glfs*, glfs_object*, char* buf, size_t bufsiz) = nullptr;
  int (*glfs_h_link)(glfs*, glfs_object* linktgt, glfs_object* parent,
                     const char* name) = nullptr;
  int (*glfs_h_rename)(glfs*, glfs_object* olddir, const char* oldname,
                       glfs_object* newdir, const char* newname) = nullptr;
  ssize_t (*glfs_h_extract_handle)(glfs_object*, unsigned char* handle, int len) = nullptr;
  glfs_object* (*glfs_h_create_from_handle)(glfs*, unsigned char* handle, int len,
                                            struct stat*) = nullptr;
  glfs_fd* (*glfs_h_opendir)(glfs*, glfs_object*) = nullptr;
  glfs_fd* (*glfs_h_open)(glfs*, glfs_object*, int flags) = nullptr;
  int (*glfs_h_access)(glfs*, glfs_object*, int mask) = nullptr;
  // fd ops
  int (*glfs_close)(glfs_fd*) = nullptr;
  int (*glfs_closedir)(glfs_fd*) = nullptr;
  int (*glfs_fstat)(glfs_fd*, struct stat*) = nullptr;
  ssize_t (*glfs_pread)(glfs_fd*, void* buf, size_t count, off_t offset, int flags,
                        struct glfs_stat* poststat) = nullptr;
  ssize_t (*glfs_pwrite)(glfs_fd*, const void* buf, size_t count, off_t offset, int flags,
                         struct glfs_stat* prestat, struct glfs_stat* poststat) = nullptr;
  ssize_t (*glfs_pwritev)(glfs_fd*, const struct iovec*, int iovcnt, off_t offset,
                          int flags) = nullptr;
  int (*glfs_fsync)(glfs_fd*, struct glfs_stat* prestat, struct glfs_stat* poststat) = nullptr;
  int (*glfs_fdatasync)(glfs_fd*, struct glfs_stat* prestat, struct glfs_stat* poststat) = nullptr;
  off_t (*glfs_lseek)(glfs_fd*, off_t offset, int whence) = nullptr;
  int (*glfs_fallocate)(glfs_fd*, int keep_size, off_t offset, size_t len) = nullptr;
  int (*glfs_discard)(glfs_fd*, off_t offset, size_t len) = nullptr;
  ssize_t (*glfs_copy_file_range)(glfs_fd* in, off64_t* off_in, glfs_fd* out,
                                  off64_t* off_out, size_t len, unsigned int flags,
                                  struct glfs_stat* statbuf, struct glfs_stat* prestat,
                                  struct glfs_stat* poststat) = nullptr;
  // directories
  int (*glfs_xreaddirplus_r)(glfs_fd*, uint32_t flags, glfs_xreaddirp_stat** xstat_p,
                             struct dirent* ext, struct dirent** res) = nullptr;
  struct stat* (*glfs_xreaddirplus_get_stat)(glfs_xreaddirp_stat*) = nullptr;
  glfs_object* (*glfs_xreaddirplus_get_object)(glfs_xreaddirp_stat*) = nullptr;
  void (*glfs_seekdir)(glfs_fd*, long offset) = nullptr;
  void (*glfs_free)(void* ptr) = nullptr;
  // byte-range locks (posix-locks xlator)
  int (*glfs_posix_lock)(glfs_fd*, int cmd, struct flock*) = nullptr;
  int (*glfs_fd_set_lkowner)(glfs_fd*, void* data, int len) = nullptr;
};

// dlopen(libgfapi.so.0) and resolve every entry of Api (versioned symbol first, plain
// name as fallback).  Errors: ENOENT when the library is not installed, ENOEXEC when
// it lacks one of the required symbols (older than GlusterFS 6).  The handle stays
// open for the life of the process (libgfapi cannot be safely unloaded).
Result<std::shared_ptr<const Api>> load_system_api(std::string* detail = nullptr);

// Every pointer set?  (The fake and the loader both go through this before use.)
bool complete(const Api& api);

}  // namespace lnfs::backend::gfapi
