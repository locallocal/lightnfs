#pragma once
// Lustre client-kernel binding for the Lustre backend (design 06 §6.5).
//
// Everything the backend needs from Lustre beyond plain POSIX is reachable through
// the kernel client — no liblustreapi at build or run time:
//   FID of an object      name_to_handle_at → FILEID_LUSTRE handle (child FID first),
//                         ioctl(LL_IOC_PATH2FID) as the fallback
//   open by FID           openat(<mount>/.lustre/fid/<seq:oid:ver>)  (what
//                         llapi_open_by_fid does; no CAP_DAC_READ_SEARCH needed,
//                         unlike open_by_handle_at)
//   HSM state / restore   ioctl(LL_IOC_HSM_STATE_GET) / ioctl(LL_IOC_HSM_REQUEST)
//   default stripe size   getxattr("lustre.lov") on the export root (lov_user_md)
//   "is this Lustre"      statfs magic LL_SUPER_MAGIC
// The uapi constants below are copied from linux/lustre/lustre_user.h (stable ABI;
// scripts/check_llapi_abi.sh static_asserts them against the real header where one
// is installed).  The Ops interface exists so tests can run the whole backend on a
// plain directory through tests/llapi_fake.cpp.

#include <cstdint>
#include <string>

#include "util/result.hpp"

namespace lnfs::backend::llapi {

struct Fid {
  uint64_t seq = 0;
  uint32_t oid = 0;
  uint32_t ver = 0;
  friend bool operator==(const Fid&, const Fid&) = default;
};
static_assert(sizeof(Fid) == 16, "struct lu_fid layout");

// name_to_handle_at handle type for Lustre; the handle body is
// struct lustre_nfs_fid { struct lu_fid lnf_child; struct lu_fid lnf_parent; }.
inline constexpr int kFileIdLustre = 0x97;
inline constexpr uint64_t kSuperMagic = 0x0BD00BD0;  // LL_SUPER_MAGIC

// HSM state bits (enum hsm_states).
inline constexpr uint32_t kHsExists = 0x01;
inline constexpr uint32_t kHsDirty = 0x02;
inline constexpr uint32_t kHsReleased = 0x04;
inline constexpr uint32_t kHsArchived = 0x08;
inline constexpr uint32_t kHsNoRelease = 0x10;
inline constexpr uint32_t kHsNoArchive = 0x20;
inline constexpr uint32_t kHsLost = 0x40;
// enum hsm_user_action
inline constexpr uint32_t kHuaRestore = 11;
// enum hsm_progress_states (hus_in_progress_state)
inline constexpr uint32_t kHpsNone = 0;
inline constexpr uint32_t kHpsWaiting = 1;
inline constexpr uint32_t kHpsRunning = 2;
inline constexpr uint32_t kHpsDone = 3;

struct HsmState {
  uint32_t states = 0;              // kHs* bits
  uint32_t archive_id = 0;
  uint32_t in_progress_state = 0;   // kHps*
  uint32_t in_progress_action = 0;  // kHua*
};

// "0x200000401:0x1:0x0" — the DFID_NOBRACE form .lustre/fid accepts.
std::string fid_to_string(const Fid& fid);

class Ops {
 public:
  virtual ~Ops() = default;
  // `fd` is an O_RDONLY directory fd on the candidate mount root.
  virtual bool is_lustre(int fd) const = 0;
  // FID of the object behind `fd` (O_PATH is fine).
  virtual Result<Fid> fid_of(int fd) const = 0;
  // openat(mount_fd, ".lustre/fid/<fid>", flags): ENOENT means the object is gone.
  virtual Result<int> open_by_fid(int mount_fd, const Fid& fid, int flags) const = 0;
  // HSM state of an open regular file (data fd, not O_PATH).  ENOTTY / EOPNOTSUPP
  // when the client has no HSM (the caller treats that as "not released").
  virtual Result<HsmState> hsm_state(int fd) const = 0;
  // Queues an HSM RESTORE for the whole file (asynchronous; the coordinator drives it).
  virtual Result<void> hsm_restore(int mount_fd, const Fid& fid) const = 0;
  // Default stripe size of the directory/file behind `fd` (data fd); ENODATA when no
  // layout is set (the filesystem default applies).
  virtual Result<uint32_t> stripe_size(int fd) const = 0;
};

// The real kernel client.
const Ops& kernel_ops();

// Parses a lov_user_md (V1/V3/composite) blob as returned by the lustre.lov xattr and
// yields the (first component's) stripe size.  Exposed for tests.
Result<uint32_t> stripe_size_from_lov(const void* data, size_t size);

}  // namespace lnfs::backend::llapi
