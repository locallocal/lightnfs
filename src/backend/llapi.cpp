#include "backend/llapi.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace lnfs::backend::llapi {
namespace {

// ---- linux/lustre/lustre_user.h subset (checked by scripts/check_llapi_abi.sh) ----

struct HsmExtent {
  uint64_t offset;
  uint64_t length;
};
struct HsmUserState {  // struct hsm_user_state
  uint32_t hus_states;
  uint32_t hus_archive_id;
  uint32_t hus_in_progress_state;
  uint32_t hus_in_progress_action;
  HsmExtent hus_in_progress_location;
};
static_assert(sizeof(HsmUserState) == 32);
struct HsmRequest {  // struct hsm_request
  uint32_t hr_action;
  uint32_t hr_archive_id;
  uint64_t hr_flags;
  uint32_t hr_itemcount;
  uint32_t hr_data_len;
};
static_assert(sizeof(HsmRequest) == 24);
struct HsmUserItem {  // struct hsm_user_item (packed upstream; naturally 32 bytes here)
  Fid hui_fid;
  HsmExtent hui_extent;
};
static_assert(sizeof(HsmUserItem) == 32);
struct HsmUserRequest {  // struct hsm_user_request (header; packed upstream, 24 bytes)
  HsmRequest hur_request;
};
static_assert(sizeof(HsmUserRequest) == 24);

constexpr unsigned long kIocPath2Fid = _IOR('f', 173, long);            // LL_IOC_PATH2FID
constexpr unsigned long kIocHsmStateGet = _IOR('f', 211, HsmUserState);   // LL_IOC_HSM_STATE_GET
constexpr unsigned long kIocHsmRequest = _IOW('f', 217, HsmUserRequest);  // LL_IOC_HSM_REQUEST

constexpr uint32_t kLovMagicV1 = 0x0BD10BD0;      // LOV_USER_MAGIC_V1
constexpr uint32_t kLovMagicV3 = 0x0BD30BD0;      // LOV_USER_MAGIC_V3
constexpr uint32_t kLovMagicCompV1 = 0x0BD60BD0;  // LOV_USER_MAGIC_COMP_V1
constexpr size_t kLovMdV1Size = 32;               // sizeof(struct lov_user_md_v1)
constexpr size_t kLovStripeSizeOffset = 24;       // offsetof(lov_user_md_v1, lmm_stripe_size)

// Lustre's struct file_handle body for FILEID_LUSTRE: child FID then parent FID.
constexpr unsigned kNfsFidBytes = 32;

uint32_t load_u32(const unsigned char* p) {
  uint32_t v = 0;
  std::memcpy(&v, p, sizeof v);
  return v;
}

std::string proc_fd_path(int fd) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "/proc/self/fd/%d", fd);
  return buf;
}

class KernelOps final : public Ops {
 public:
  bool is_lustre(int fd) const override {
    struct statfs sf {};
    return ::fstatfs(fd, &sf) == 0 && static_cast<uint64_t>(sf.f_type) == kSuperMagic;
  }

  Result<Fid> fid_of(int fd) const override {
    // Primary: the NFS export handle.  Unprivileged, works on O_PATH fds and on every
    // object type (symlinks and devices included).
    alignas(8) unsigned char storage[sizeof(file_handle) + kNfsFidBytes];
    auto* handle = reinterpret_cast<file_handle*>(storage);
    handle->handle_bytes = kNfsFidBytes;
    int mount_id = 0;
    if (::name_to_handle_at(fd, "", handle, &mount_id, AT_EMPTY_PATH) == 0 &&
        handle->handle_type == kFileIdLustre && handle->handle_bytes >= sizeof(Fid)) {
      Fid out;
      std::memcpy(&out, handle->f_handle, sizeof out);
      return out;
    }
    // Fallback: LL_IOC_PATH2FID needs a real (non-O_PATH) descriptor; reopen through
    // /proc (regular files and directories only).
    int real = ::open(proc_fd_path(fd).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (real < 0) return Err(errno_from(errno == ELOOP ? EOPNOTSUPP : errno));
    Fid out;
    int rc = ::ioctl(real, kIocPath2Fid, &out);
    int e = errno;
    ::close(real);
    if (rc < 0) return Err(errno_from(e == ENOTTY ? EOPNOTSUPP : e));
    return out;
  }

  Result<int> open_by_fid(int mount_fd, const Fid& fid, int flags) const override {
    std::string path = ".lustre/fid/" + fid_to_string(fid);
    int fd = ::openat(mount_fd, path.c_str(), flags | O_CLOEXEC);
    if (fd < 0) return Err(errno_from(errno));
    return fd;
  }

  Result<HsmState> hsm_state(int fd) const override {
    HsmUserState hus{};
    if (::ioctl(fd, kIocHsmStateGet, &hus) < 0) return Err(errno_from(errno));
    return HsmState{hus.hus_states, hus.hus_archive_id, hus.hus_in_progress_state,
                    hus.hus_in_progress_action};
  }

  Result<void> hsm_restore(int mount_fd, const Fid& fid) const override {
    alignas(8) unsigned char buf[sizeof(HsmUserRequest) + sizeof(HsmUserItem)] = {};
    HsmUserRequest req{};
    req.hur_request.hr_action = kHuaRestore;
    req.hur_request.hr_itemcount = 1;
    HsmUserItem item{};
    item.hui_fid = fid;
    item.hui_extent.offset = 0;
    item.hui_extent.length = UINT64_MAX;  // whole file
    std::memcpy(buf, &req, sizeof req);
    std::memcpy(buf + sizeof req, &item, sizeof item);
    if (::ioctl(mount_fd, kIocHsmRequest, buf) < 0) return Err(errno_from(errno));
    return {};
  }

  Result<uint32_t> stripe_size(int fd) const override {
    std::vector<unsigned char> buf(4096);
    for (;;) {
      ssize_t n = ::fgetxattr(fd, "lustre.lov", buf.data(), buf.size());
      if (n >= 0) return stripe_size_from_lov(buf.data(), static_cast<size_t>(n));
      if (errno != ERANGE || buf.size() >= (1u << 16)) return Err(errno_from(errno));
      buf.resize(buf.size() * 4);
    }
  }
};

}  // namespace

std::string fid_to_string(const Fid& fid) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "0x%llx:0x%x:0x%x", static_cast<unsigned long long>(fid.seq),
                fid.oid, fid.ver);
  return buf;
}

Result<uint32_t> stripe_size_from_lov(const void* data, size_t size) {
  const auto* p = static_cast<const unsigned char*>(data);
  if (size < 4) return Err(errno_from(ENODATA));
  uint32_t magic = load_u32(p);
  if (magic == kLovMagicV1 || magic == kLovMagicV3) {
    if (size < kLovMdV1Size) return Err(errno_from(EINVAL));
    uint32_t stripe = load_u32(p + kLovStripeSizeOffset);
    if (stripe == 0) return Err(errno_from(ENODATA));  // "filesystem default"
    return stripe;
  }
  if (magic == kLovMagicCompV1) {
    // Composite (PFL/FLR) layout: components are lov_user_md blobs placed at the
    // entries' lcme_offset.  The first V1/V3 magic after the header is component 0,
    // which is the one that shapes the initial write pattern.
    for (size_t off = 8; off + kLovMdV1Size <= size; off += 4) {
      uint32_t m = load_u32(p + off);
      if (m == kLovMagicV1 || m == kLovMagicV3) {
        uint32_t stripe = load_u32(p + off + kLovStripeSizeOffset);
        if (stripe == 0) return Err(errno_from(ENODATA));
        return stripe;
      }
    }
    return Err(errno_from(ENODATA));
  }
  return Err(errno_from(ENODATA));  // foreign / unknown layout: use defaults
}

const Ops& kernel_ops() {
  static const KernelOps ops;
  return ops;
}

}  // namespace lnfs::backend::llapi
