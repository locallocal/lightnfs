#include "backend/gfapi.hpp"

#include <dlfcn.h>

#include <cerrno>
#include <cstring>
#include <mutex>

namespace lnfs::backend::gfapi {
namespace {

struct Sym {
  const char* name;
  const char* version;  // GFAPI_x.y.z default version in GlusterFS 11's map file
  size_t offset;        // of the member in Api
};

#define LNFS_GFAPI_SYM(fn, ver) Sym{#fn, "GFAPI_" ver, offsetof(Api, fn)}
constexpr Sym kSyms[] = {
    LNFS_GFAPI_SYM(glfs_new, "3.4.0"),
    LNFS_GFAPI_SYM(glfs_set_volfile_server, "3.4.0"),
    LNFS_GFAPI_SYM(glfs_set_logging, "3.4.0"),
    LNFS_GFAPI_SYM(glfs_init, "3.4.0"),
    LNFS_GFAPI_SYM(glfs_fini, "3.4.0"),
    LNFS_GFAPI_SYM(glfs_get_volumeid, "3.5.0"),
    LNFS_GFAPI_SYM(glfs_setfsuid, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_setfsgid, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_setfsgroups, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_lookupat, "3.7.4"),
    LNFS_GFAPI_SYM(glfs_h_creat, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_mkdir, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_mknod, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_symlink, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_unlink, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_close, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_truncate, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_stat, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_statfs, "3.7.0"),
    LNFS_GFAPI_SYM(glfs_h_setattrs, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_readlink, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_link, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_rename, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_extract_handle, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_create_from_handle, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_opendir, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_open, "3.4.2"),
    LNFS_GFAPI_SYM(glfs_h_access, "3.6.0"),
    LNFS_GFAPI_SYM(glfs_close, "3.4.0"),
    LNFS_GFAPI_SYM(glfs_closedir, "3.4.0"),
    LNFS_GFAPI_SYM(glfs_fstat, "3.4.0"),
    LNFS_GFAPI_SYM(glfs_pread, "6.0"),
    LNFS_GFAPI_SYM(glfs_pwrite, "6.0"),
    LNFS_GFAPI_SYM(glfs_pwritev, "3.4.0"),
    LNFS_GFAPI_SYM(glfs_fsync, "6.0"),
    LNFS_GFAPI_SYM(glfs_fdatasync, "6.0"),
    LNFS_GFAPI_SYM(glfs_lseek, "3.4.0"),
    LNFS_GFAPI_SYM(glfs_fallocate, "3.5.0"),
    LNFS_GFAPI_SYM(glfs_discard, "3.5.0"),
    LNFS_GFAPI_SYM(glfs_copy_file_range, "6.0"),
    LNFS_GFAPI_SYM(glfs_xreaddirplus_r, "3.11.0"),
    LNFS_GFAPI_SYM(glfs_xreaddirplus_get_stat, "3.11.0"),
    LNFS_GFAPI_SYM(glfs_xreaddirplus_get_object, "3.11.0"),
    LNFS_GFAPI_SYM(glfs_seekdir, "3.4.0"),
    LNFS_GFAPI_SYM(glfs_free, "3.7.16"),
    LNFS_GFAPI_SYM(glfs_posix_lock, "3.4.0"),
    LNFS_GFAPI_SYM(glfs_fd_set_lkowner, "3.10.7"),
};
#undef LNFS_GFAPI_SYM

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
  const char* candidates[] = {"libgfapi.so.0", "libgfapi.so"};
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
    void* fn = ::dlvsym(handle, s.name, s.version);
    if (!fn) fn = ::dlsym(handle, s.name);
    if (!fn) {
      cached_error = errno_from(ENOEXEC);
      cached_detail = std::string("missing symbol ") + s.name + " (GlusterFS >= 6 required)";
      if (detail) *detail = cached_detail;
      return Err(cached_error);
    }
    std::memcpy(reinterpret_cast<char*>(api.get()) + s.offset, &fn, sizeof fn);
  }
  cached = api;
  return cached;
}

}  // namespace lnfs::backend::gfapi
