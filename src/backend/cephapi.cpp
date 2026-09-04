#include "backend/cephapi.hpp"

#include <dlfcn.h>

#include <cerrno>
#include <cstring>
#include <mutex>

namespace lnfs::backend::cephapi {
namespace {

struct Sym {
  const char* name;
  size_t offset;  // of the member in Api
};

// libcephfs exports unversioned symbols (no version script), so plain dlsym.
#define LNFS_CEPH_SYM(fn) Sym{#fn, offsetof(Api, fn)}
constexpr Sym kSyms[] = {
    LNFS_CEPH_SYM(ceph_version),
    LNFS_CEPH_SYM(ceph_create),
    LNFS_CEPH_SYM(ceph_conf_read_file),
    LNFS_CEPH_SYM(ceph_conf_set),
    LNFS_CEPH_SYM(ceph_conf_get),
    LNFS_CEPH_SYM(ceph_init),
    LNFS_CEPH_SYM(ceph_select_filesystem),
    LNFS_CEPH_SYM(ceph_mount),
    LNFS_CEPH_SYM(ceph_unmount),
    LNFS_CEPH_SYM(ceph_release),
    LNFS_CEPH_SYM(ceph_shutdown),
    LNFS_CEPH_SYM(ceph_get_fs_cid),
    LNFS_CEPH_SYM(ceph_get_instance_id),
    LNFS_CEPH_SYM(ceph_userperm_new),
    LNFS_CEPH_SYM(ceph_userperm_destroy),
    LNFS_CEPH_SYM(ceph_ll_lookup_root),
    LNFS_CEPH_SYM(ceph_ll_lookup_vino),
    LNFS_CEPH_SYM(ceph_ll_lookup),
    LNFS_CEPH_SYM(ceph_ll_put),
    LNFS_CEPH_SYM(ceph_ll_getattr),
    LNFS_CEPH_SYM(ceph_ll_setattr),
    LNFS_CEPH_SYM(ceph_ll_open),
    LNFS_CEPH_SYM(ceph_ll_create),
    LNFS_CEPH_SYM(ceph_ll_mknod),
    LNFS_CEPH_SYM(ceph_ll_mkdir),
    LNFS_CEPH_SYM(ceph_ll_symlink),
    LNFS_CEPH_SYM(ceph_ll_link),
    LNFS_CEPH_SYM(ceph_ll_unlink),
    LNFS_CEPH_SYM(ceph_ll_rmdir),
    LNFS_CEPH_SYM(ceph_ll_rename),
    LNFS_CEPH_SYM(ceph_ll_readlink),
    LNFS_CEPH_SYM(ceph_ll_statfs),
    LNFS_CEPH_SYM(ceph_ll_stripe_unit),
    LNFS_CEPH_SYM(ceph_ll_read),
    LNFS_CEPH_SYM(ceph_ll_write),
    LNFS_CEPH_SYM(ceph_ll_writev),
    LNFS_CEPH_SYM(ceph_ll_fsync),
    LNFS_CEPH_SYM(ceph_ll_lseek),
    LNFS_CEPH_SYM(ceph_ll_fallocate),
    LNFS_CEPH_SYM(ceph_ll_close),
    LNFS_CEPH_SYM(ceph_ll_getlk),
    LNFS_CEPH_SYM(ceph_ll_setlk),
    LNFS_CEPH_SYM(ceph_ll_opendir),
    LNFS_CEPH_SYM(ceph_ll_releasedir),
    LNFS_CEPH_SYM(ceph_readdirplus_r),
    LNFS_CEPH_SYM(ceph_seekdir),
};
#undef LNFS_CEPH_SYM

}  // namespace

bool complete(const Api& api) {
  for (const Sym& s : kSyms) {
    void* fn = nullptr;
    std::memcpy(&fn, reinterpret_cast<const char*>(&api) + s.offset, sizeof fn);
    if (!fn) return false;
  }
  return true;
}

Result<std::shared_ptr<const Api>> load_system_api(std::string* detail) {
  static std::mutex mu;
  static std::shared_ptr<const Api> cached;
  static Errno cached_error = Errno::kOk;
  static std::string cached_detail;
  std::lock_guard lock(mu);
  if (cached) return cached;
  if (cached_error != Errno::kOk) {
    if (detail) *detail = cached_detail;
    return Err(cached_error);
  }
  const char* candidates[] = {"libcephfs.so.2", "libcephfs.so"};
  void* handle = nullptr;
  for (const char* name : candidates) {
    handle = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (handle) break;
  }
  if (!handle) {
    cached_error = errno_from(ENOENT);
    const char* err = ::dlerror();
    cached_detail = err ? err : "dlopen failed";
    if (detail) *detail = cached_detail;
    return Err(cached_error);
  }
  auto api = std::make_shared<Api>();
  for (const Sym& s : kSyms) {
    void* fn = ::dlsym(handle, s.name);
    if (!fn) {
      cached_error = errno_from(ENOEXEC);
      cached_detail = std::string("missing symbol ") + s.name + " (Ceph >= 15 required)";
      if (detail) *detail = cached_detail;
      return Err(cached_error);
    }
    std::memcpy(reinterpret_cast<char*>(api.get()) + s.offset, &fn, sizeof fn);
  }
  cached = api;
  return cached;
}

}  // namespace lnfs::backend::cephapi
