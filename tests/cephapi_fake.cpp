#include "cephapi_fake.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

// The opaque types are declared in the global namespace by backend/cephapi.hpp; the
// fake defines them.
struct ceph_mount_info {
  std::string id;
  std::string root;  // the directory bound at ceph_mount
  int root_fd = -1;
  bool inited = false;
  bool mounted = false;
  std::string fs_name;
  std::unordered_map<std::string, std::string> conf;
};

struct Inode {
  ceph_mount_info* mount = nullptr;
  int fd = -1;  // O_PATH
  uint64_t ino = 0;  // synthetic, never reused
};

class Fh {
 public:
  ceph_mount_info* mount = nullptr;
  int fd = -1;
  uint64_t ino = 0;
  int flags = 0;
};

struct UserPerm {
  uint32_t uid = 0;
  uint32_t gid = 0;
  std::vector<gid_t> groups;
};

struct ceph_dir_result {
  ceph_mount_info* mount = nullptr;
  DIR* dir = nullptr;
  int dirfd = -1;
  uint64_t ino = 0;
};

namespace {

using lnfs::backend::cephapi::Api;
namespace capi = lnfs::backend::cephapi;

struct State {
  std::mutex mu;
  std::string root;
  int fail_mount = 0;
  std::atomic<int> fail_left{0};
  std::atomic<int> fail_err{0};
  std::atomic<int> live_inodes{0};
  std::atomic<int> live_fhs{0};
  std::atomic<int> live_dirs{0};
  std::atomic<int> live_perms{0};
  std::atomic<uint64_t> getattr_calls{0};
  // Ceph never reuses inode numbers: real (dev, ino) → synthetic ino, assigned fresh
  // on every creation; synthetic ino → relative path (the "MDS lookup_ino" table)
  // and → change counter (stx_version).
  struct Real {
    uint64_t dev, ino;
    friend bool operator==(const Real&, const Real&) = default;
  };
  struct RealHash {
    size_t operator()(const Real& r) const noexcept { return r.dev * 1315423911u ^ r.ino; }
  };
  std::unordered_map<Real, uint64_t, RealHash> synthetic;
  std::unordered_map<uint64_t, std::string> paths;
  std::unordered_map<uint64_t, uint64_t> versions;
  uint64_t next_ino = 0x10000000000ull;  // Ceph inode numbers start high too
  // fcntl locks: (ino, owner) segments, remembered per Fh
  struct Seg {
    uint64_t ino;
    uint64_t owner;
    Fh* fh;
    uint64_t start, end;  // end exclusive, UINT64_MAX = EOF
    bool excl;
  };
  std::vector<Seg> locks;
};

State& st() {
  static State s;
  return s;
}

std::atomic<uint32_t> g_last_uid{0}, g_last_gid{0};

void note_identity(const UserPerm* p) {
  if (!p) return;
  g_last_uid.store(p->uid, std::memory_order_relaxed);
  g_last_gid.store(p->gid, std::memory_order_relaxed);
}

// Injected failure for the next N calls.
bool take_fail(int& err) {
  auto& s = st();
  int cur = s.fail_left.load();
  while (cur > 0) {
    if (s.fail_left.compare_exchange_weak(cur, cur - 1)) {
      err = s.fail_err.load();
      return true;
    }
  }
  return false;
}
#define FAKE_FAIL()            \
  do {                         \
    int e_ = 0;                \
    if (take_fail(e_)) return -e_; \
  } while (0)

std::string proc_path(int fd) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "/proc/self/fd/%d", fd);
  return buf;
}

std::string join(const std::string& parent, const std::string& name) {
  if (name == ".") return parent;
  if (name == "..") {
    auto slash = parent.rfind('/');
    return slash == std::string::npos ? std::string(".") : parent.substr(0, slash);
  }
  return parent == "." ? name : parent + "/" + name;
}

// Synthetic inode number for a real (dev, ino); `created` forces a fresh one.
uint64_t synthetic_ino(const struct stat& sb, const std::string& path, bool created) {
  auto& s = st();
  std::lock_guard lock(s.mu);
  State::Real key{sb.st_dev, sb.st_ino};
  auto it = s.synthetic.find(key);
  if (created || it == s.synthetic.end()) {
    uint64_t ino = s.next_ino++;
    s.synthetic[key] = ino;
    s.paths[ino] = path;
    s.versions[ino] = 1;
    return ino;
  }
  s.paths[it->second] = path;
  return it->second;
}

void bump_version(uint64_t ino) {
  auto& s = st();
  std::lock_guard lock(s.mu);
  ++s.versions[ino];
}

uint64_t version_of(uint64_t ino) {
  auto& s = st();
  std::lock_guard lock(s.mu);
  auto it = s.versions.find(ino);
  return it == s.versions.end() ? 0 : it->second;
}

std::string path_of(uint64_t ino) {
  auto& s = st();
  std::lock_guard lock(s.mu);
  auto it = s.paths.find(ino);
  return it == s.paths.end() ? std::string() : it->second;
}

// Materializes an Inode for an O_PATH fd (adopts fd).
Inode* make_inode(ceph_mount_info* m, int fd, const std::string& path, bool created) {
  struct stat sb {};
  if (fstat(fd, &sb) < 0) {
    close(fd);
    return nullptr;
  }
  auto* in = new Inode;
  in->mount = m;
  in->fd = fd;
  in->ino = synthetic_ino(sb, path, created);
  st().live_inodes.fetch_add(1);
  return in;
}

void fill_statx(const struct stat& sb, uint64_t ino, struct ceph_statx* stx) {
  if (!stx) return;
  *stx = {};
  stx->stx_mask = capi::kStatxAllStats;
  stx->stx_blksize = static_cast<uint32_t>(sb.st_blksize);
  stx->stx_nlink = static_cast<uint32_t>(sb.st_nlink);
  stx->stx_uid = sb.st_uid;
  stx->stx_gid = sb.st_gid;
  stx->stx_mode = static_cast<uint16_t>(sb.st_mode);
  stx->stx_ino = ino;
  stx->stx_size = static_cast<uint64_t>(sb.st_size);
  stx->stx_blocks = static_cast<uint64_t>(sb.st_blocks);
  stx->stx_dev = static_cast<dev_t>(capi::kNoSnap);  // the snapid, as libcephfs reports it
  stx->stx_rdev = sb.st_rdev;
  stx->stx_atime = sb.st_atim;
  stx->stx_ctime = sb.st_ctim;
  stx->stx_mtime = sb.st_mtim;
  stx->stx_btime = sb.st_ctim;
  stx->stx_version = version_of(ino);
}

int stat_inode(Inode* in, struct stat* out) {
  return fstatat(in->fd, "", out, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
}

// Permission model of client_permissions: mode bits under the UserPerm; uid 0 may do
// anything (execute needs some x bit, like the real client).
bool permits(const struct stat& sb, int mode, const UserPerm* p) {
  if (!p) return true;
  if (p->uid == 0) return !(mode & X_OK) || S_ISDIR(sb.st_mode) || (sb.st_mode & 0111);
  int shift = 0;
  if (p->uid == sb.st_uid) shift = 6;
  else if (p->gid == sb.st_gid ||
           std::find(p->groups.begin(), p->groups.end(), sb.st_gid) != p->groups.end())
    shift = 3;
  int bits = (sb.st_mode >> shift) & 7;
  if ((mode & R_OK) && !(bits & 4)) return false;
  if ((mode & W_OK) && !(bits & 2)) return false;
  if ((mode & X_OK) && !(bits & 1)) return false;
  return true;
}

int check_dir(Inode* dir, int mode, const UserPerm* p) {
  struct stat sb {};
  if (stat_inode(dir, &sb) < 0) return -errno;
  if (!S_ISDIR(sb.st_mode)) return -ENOTDIR;
  if (!permits(sb, mode, p)) return -EACCES;
  return 0;
}

// Opens `name` under `parent` as a new Inode (created → fresh synthetic ino).
int child_inode(Inode* parent, const char* name, bool created, Inode** out, struct ceph_statx* stx) {
  int fd = openat(parent->fd, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return -errno;
  Inode* in = make_inode(parent->mount, fd, join(path_of(parent->ino), name), created);
  if (!in) return -EIO;
  struct stat sb {};
  if (stat_inode(in, &sb) < 0) {
    int e = errno;
    close(in->fd);
    delete in;
    st().live_inodes.fetch_sub(1);
    return -e;
  }
  fill_statx(sb, in->ino, stx);
  *out = in;
  return 0;
}

// ---- lifecycle ----------------------------------------------------------------

const char* f_version(int* major, int* minor, int* patch) {
  if (major) *major = 20;
  if (minor) *minor = 2;
  if (patch) *patch = 0;
  return "fake-libcephfs 20.2.0";
}
int f_create(ceph_mount_info** out, const char* const id) {
  auto* m = new ceph_mount_info;
  m->id = id ? id : "admin";
  *out = m;
  return 0;
}
int f_conf_read_file(ceph_mount_info* m, const char* path) {
  if (!path) return -ENOENT;  // no default ceph.conf on this host
  if (access(path, R_OK) != 0) return -errno;
  m->conf["conf"] = path;
  return 0;
}
int f_conf_set(ceph_mount_info* m, const char* key, const char* value) {
  if (!key || !value) return -EINVAL;
  m->conf[key] = value;
  return 0;
}
int f_conf_get(ceph_mount_info* m, const char* key, char* buf, size_t len) {
  std::string v;
  if (std::string(key) == "fsid") v = "0f7a3b7e-fake-4c0d-9d1c-0123456789ab";
  else {
    auto it = m->conf.find(key);
    if (it == m->conf.end()) return -ENOENT;
    v = it->second;
  }
  if (v.size() + 1 > len) return -ENAMETOOLONG;
  std::memcpy(buf, v.c_str(), v.size() + 1);
  return static_cast<int>(v.size());
}
int f_init(ceph_mount_info* m) {
  m->inited = true;
  return 0;
}
int f_select_filesystem(ceph_mount_info* m, const char* fs) {
  if (m->mounted) return -EISCONN;
  m->fs_name = fs ? fs : "";
  return 0;
}
int f_mount(ceph_mount_info* m, const char* root) {
  auto& s = st();
  std::string base;
  int fail = 0;
  {
    std::lock_guard lock(s.mu);
    base = s.root;
    fail = s.fail_mount;
  }
  if (fail) return -fail;
  std::string path = base;
  if (root && *root && std::string(root) != "/") path += root;
  int fd = open(path.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return -errno;
  m->root = path;
  m->root_fd = fd;
  m->mounted = true;
  return 0;
}
int f_unmount(ceph_mount_info* m) {
  if (!m->mounted) return -ENOTCONN;
  if (m->root_fd >= 0) close(m->root_fd);
  m->root_fd = -1;
  m->mounted = false;
  return 0;
}
int f_release(ceph_mount_info* m) {
  if (m->mounted) return -EISCONN;
  delete m;
  return 0;
}
void f_shutdown(ceph_mount_info* m) {
  if (m->mounted) f_unmount(m);
  f_release(m);
}
int64_t f_get_fs_cid(ceph_mount_info*) { return 7; }
uint64_t f_get_instance_id(ceph_mount_info*) { return 4242; }

UserPerm* f_userperm_new(uid_t uid, gid_t gid, int ngids, gid_t* gids) {
  auto* p = new UserPerm;
  p->uid = uid;
  p->gid = gid;
  if (ngids > 0 && gids) p->groups.assign(gids, gids + ngids);
  st().live_perms.fetch_add(1);
  return p;
}
void f_userperm_destroy(UserPerm* p) {
  if (!p) return;
  st().live_perms.fetch_sub(1);
  delete p;
}

// ---- inodes ----------------------------------------------------------------------

int f_ll_lookup_root(ceph_mount_info* m, Inode** out) {
  if (!m->mounted) return -ENOTCONN;
  int fd = dup(m->root_fd);
  if (fd < 0) return -errno;
  Inode* in = make_inode(m, fd, ".", false);
  if (!in) return -EIO;
  *out = in;
  return 0;
}

int f_ll_lookup_vino(ceph_mount_info* m, vinodeno_t vino, Inode** out) {
  FAKE_FAIL();
  if (vino.snapid != capi::kNoSnap) return -ENOENT;  // no snapshots in the fake
  std::string rel = path_of(vino.ino);
  if (rel.empty()) return -ENOENT;
  int fd = openat(m->root_fd, rel.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return -ENOENT;
  struct stat sb {};
  if (fstat(fd, &sb) < 0) {
    close(fd);
    return -ENOENT;
  }
  {
    auto& s = st();
    std::lock_guard lock(s.mu);
    auto it = s.synthetic.find(State::Real{sb.st_dev, sb.st_ino});
    if (it == s.synthetic.end() || it->second != vino.ino) {
      close(fd);
      return -ENOENT;  // the path now holds a different (re-created) inode
    }
  }
  auto* in = new Inode;
  in->mount = m;
  in->fd = fd;
  in->ino = vino.ino;
  st().live_inodes.fetch_add(1);
  *out = in;
  return 0;
}

int f_ll_lookup(ceph_mount_info*, Inode* parent, const char* name, Inode** out,
                struct ceph_statx* stx, unsigned, unsigned, const UserPerm* perms) {
  note_identity(perms);
  FAKE_FAIL();
  int rc = check_dir(parent, X_OK, perms);
  if (rc < 0) return rc;
  return child_inode(parent, name, false, out, stx);
}

int f_ll_put(ceph_mount_info*, Inode* in) {
  if (!in) return -EINVAL;
  if (in->fd >= 0) close(in->fd);
  st().live_inodes.fetch_sub(1);
  delete in;
  return 0;
}

int f_ll_getattr(ceph_mount_info*, Inode* in, struct ceph_statx* stx, unsigned, unsigned,
                 const UserPerm* perms) {
  note_identity(perms);
  st().getattr_calls.fetch_add(1);
  FAKE_FAIL();
  struct stat sb {};
  if (stat_inode(in, &sb) < 0) return -errno;
  fill_statx(sb, in->ino, stx);
  return 0;
}

int f_ll_setattr(ceph_mount_info*, Inode* in, struct ceph_statx* stx, int mask,
                 const UserPerm* perms) {
  note_identity(perms);
  FAKE_FAIL();
  struct stat sb {};
  if (stat_inode(in, &sb) < 0) return -errno;
  bool owner = !perms || perms->uid == 0 || perms->uid == sb.st_uid;
  std::string p = proc_path(in->fd);
  if (mask & capi::kSetSize) {
    if (!permits(sb, W_OK, perms) && !owner) return -EACCES;
    int fd = open(p.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0 && errno == EACCES && (!perms || perms->uid == 0 || perms->uid == sb.st_uid)) {
      // this test process is not root: widen the mode briefly (tests only)
      if (chmod(p.c_str(), (sb.st_mode & 07777) | 0200) == 0) {
        fd = open(p.c_str(), O_WRONLY | O_CLOEXEC);
        (void)chmod(p.c_str(), sb.st_mode & 07777);
      }
    }
    if (fd < 0) return -errno;
    if (ftruncate(fd, static_cast<off_t>(stx->stx_size)) < 0) {
      int e = errno;
      close(fd);
      return -e;
    }
    close(fd);
  }
  if (mask & (capi::kSetMode | capi::kSetUid | capi::kSetGid | capi::kSetAtime |
              capi::kSetMtime | capi::kSetAtimeNow | capi::kSetMtimeNow) &&
      !owner)
    return -EPERM;
  if ((mask & capi::kSetMode) && chmod(p.c_str(), stx->stx_mode & 07777) < 0) return -errno;
  if (mask & (capi::kSetUid | capi::kSetGid)) {
    uid_t uid = (mask & capi::kSetUid) ? stx->stx_uid : static_cast<uid_t>(-1);
    gid_t gid = (mask & capi::kSetGid) ? stx->stx_gid : static_cast<gid_t>(-1);
    if (fchownat(in->fd, "", uid, gid, AT_EMPTY_PATH) < 0) return -errno;
  }
  if (mask & (capi::kSetAtime | capi::kSetMtime | capi::kSetAtimeNow | capi::kSetMtimeNow)) {
    timespec times[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};
    if (mask & capi::kSetAtimeNow) times[0] = {0, UTIME_NOW};
    else if (mask & capi::kSetAtime) times[0] = stx->stx_atime;
    if (mask & capi::kSetMtimeNow) times[1] = {0, UTIME_NOW};
    else if (mask & capi::kSetMtime) times[1] = stx->stx_mtime;
    if (utimensat(AT_FDCWD, p.c_str(), times, 0) < 0) return -errno;
  }
  bump_version(in->ino);
  return 0;
}

int f_ll_open(ceph_mount_info* m, Inode* in, int flags, Fh** out, const UserPerm* perms) {
  note_identity(perms);
  FAKE_FAIL();
  struct stat sb {};
  if (stat_inode(in, &sb) < 0) return -errno;
  if (S_ISDIR(sb.st_mode)) return -EISDIR;
  int acc = flags & O_ACCMODE;
  int need = acc == O_RDONLY ? R_OK : acc == O_WRONLY ? W_OK : R_OK | W_OK;
  if (!permits(sb, need, perms)) return -EACCES;
  int fd = open(proc_path(in->fd).c_str(), flags | O_CLOEXEC);
  if (fd < 0 && errno == EACCES && perms && perms->uid == 0) {
    // The MDS honours uid 0 regardless of mode bits; this test process does not, so
    // emulate that for the gateway identity by briefly widening the mode (tests only).
    std::string p = proc_path(in->fd);
    if (chmod(p.c_str(), (sb.st_mode & 07777) | 0600) == 0) {
      fd = open(p.c_str(), flags | O_CLOEXEC);
      int e = errno;
      (void)chmod(p.c_str(), sb.st_mode & 07777);
      errno = e;
    }
  }
  if (fd < 0) return -errno;
  auto* fh = new Fh;
  fh->mount = m;
  fh->fd = fd;
  fh->ino = in->ino;
  fh->flags = flags;
  st().live_fhs.fetch_add(1);
  if (flags & O_TRUNC) bump_version(in->ino);
  *out = fh;
  return 0;
}

int f_ll_create(ceph_mount_info* m, Inode* parent, const char* name, mode_t mode, int oflags,
                Inode** outp, Fh** fhp, struct ceph_statx* stx, unsigned, unsigned,
                const UserPerm* perms) {
  note_identity(perms);
  FAKE_FAIL();
  int rc = check_dir(parent, W_OK | X_OK, perms);
  if (rc < 0) return rc;
  int fd = openat(parent->fd, name, oflags | O_CLOEXEC | O_NOFOLLOW, mode);
  if (fd < 0) return -errno;
  bool created = (oflags & O_CREAT) != 0;
  Inode* in = nullptr;
  rc = child_inode(parent, name, created, &in, stx);
  if (rc < 0) {
    close(fd);
    return rc;
  }
  if (created) bump_version(parent->ino);
  auto* fh = new Fh;
  fh->mount = m;
  fh->fd = fd;
  fh->ino = in->ino;
  fh->flags = oflags;
  st().live_fhs.fetch_add(1);
  *outp = in;
  if (fhp) *fhp = fh;
  else f_ll_put(m, in), delete fh;
  return 0;
}

int f_ll_mknod(ceph_mount_info*, Inode* parent, const char* name, mode_t mode, dev_t rdev,
               Inode** out, struct ceph_statx* stx, unsigned, unsigned, const UserPerm* perms) {
  note_identity(perms);
  FAKE_FAIL();
  int rc = check_dir(parent, W_OK | X_OK, perms);
  if (rc < 0) return rc;
  if (mknodat(parent->fd, name, mode, rdev) < 0) return -errno;
  bump_version(parent->ino);
  return child_inode(parent, name, true, out, stx);
}

int f_ll_mkdir(ceph_mount_info*, Inode* parent, const char* name, mode_t mode, Inode** out,
               struct ceph_statx* stx, unsigned, unsigned, const UserPerm* perms) {
  note_identity(perms);
  FAKE_FAIL();
  int rc = check_dir(parent, W_OK | X_OK, perms);
  if (rc < 0) return rc;
  if (mkdirat(parent->fd, name, mode) < 0) return -errno;
  bump_version(parent->ino);
  return child_inode(parent, name, true, out, stx);
}

int f_ll_symlink(ceph_mount_info*, Inode* parent, const char* name, const char* value,
                 Inode** out, struct ceph_statx* stx, unsigned, unsigned, const UserPerm* perms) {
  note_identity(perms);
  FAKE_FAIL();
  int rc = check_dir(parent, W_OK | X_OK, perms);
  if (rc < 0) return rc;
  if (symlinkat(value, parent->fd, name) < 0) return -errno;
  bump_version(parent->ino);
  return child_inode(parent, name, true, out, stx);
}

int f_ll_link(ceph_mount_info*, Inode* in, Inode* newparent, const char* name,
              const UserPerm* perms) {
  note_identity(perms);
  FAKE_FAIL();
  int rc = check_dir(newparent, W_OK | X_OK, perms);
  if (rc < 0) return rc;
  if (linkat(AT_FDCWD, proc_path(in->fd).c_str(), newparent->fd, name, AT_SYMLINK_FOLLOW) < 0)
    return -errno;
  bump_version(in->ino);
  bump_version(newparent->ino);
  return 0;
}

int f_ll_unlink(ceph_mount_info*, Inode* parent, const char* name, const UserPerm* perms) {
  note_identity(perms);
  FAKE_FAIL();
  int rc = check_dir(parent, W_OK | X_OK, perms);
  if (rc < 0) return rc;
  struct stat sb {};
  if (fstatat(parent->fd, name, &sb, AT_SYMLINK_NOFOLLOW) < 0) return -errno;
  if (S_ISDIR(sb.st_mode)) return -EISDIR;
  if (unlinkat(parent->fd, name, 0) < 0) return -errno;
  {
    auto& s = st();
    std::lock_guard lock(s.mu);
    auto it = s.synthetic.find(State::Real{sb.st_dev, sb.st_ino});
    if (it != s.synthetic.end()) {
      ++s.versions[it->second];
      if (sb.st_nlink <= 1) {  // last link gone: the inode number is retired
        s.paths.erase(it->second);
        s.synthetic.erase(it);
      }
    }
  }
  bump_version(parent->ino);
  return 0;
}

int f_ll_rmdir(ceph_mount_info*, Inode* parent, const char* name, const UserPerm* perms) {
  note_identity(perms);
  FAKE_FAIL();
  int rc = check_dir(parent, W_OK | X_OK, perms);
  if (rc < 0) return rc;
  struct stat sb {};
  if (fstatat(parent->fd, name, &sb, AT_SYMLINK_NOFOLLOW) < 0) return -errno;
  if (!S_ISDIR(sb.st_mode)) return -ENOTDIR;
  if (unlinkat(parent->fd, name, AT_REMOVEDIR) < 0) return -errno;
  {
    auto& s = st();
    std::lock_guard lock(s.mu);
    auto it = s.synthetic.find(State::Real{sb.st_dev, sb.st_ino});
    if (it != s.synthetic.end()) {
      s.paths.erase(it->second);
      s.synthetic.erase(it);
    }
  }
  bump_version(parent->ino);
  return 0;
}

int f_ll_rename(ceph_mount_info*, Inode* parent, const char* name, Inode* newparent,
                const char* newname, const UserPerm* perms) {
  note_identity(perms);
  FAKE_FAIL();
  int rc = check_dir(parent, W_OK | X_OK, perms);
  if (rc < 0) return rc;
  rc = check_dir(newparent, W_OK | X_OK, perms);
  if (rc < 0) return rc;
  struct stat victim {};
  bool had_victim = fstatat(newparent->fd, newname, &victim, AT_SYMLINK_NOFOLLOW) == 0;
  if (renameat(parent->fd, name, newparent->fd, newname) < 0) return -errno;
  struct stat sb {};
  if (fstatat(newparent->fd, newname, &sb, AT_SYMLINK_NOFOLLOW) == 0) {
    std::string moved = join(path_of(newparent->ino), newname);
    auto& s = st();
    std::lock_guard lock(s.mu);
    auto it = s.synthetic.find(State::Real{sb.st_dev, sb.st_ino});
    if (it != s.synthetic.end()) {
      s.paths[it->second] = moved;
      ++s.versions[it->second];
    }
    if (had_victim && victim.st_nlink <= 1) {
      auto v = s.synthetic.find(State::Real{victim.st_dev, victim.st_ino});
      if (v != s.synthetic.end() && v->second != it->second) {
        s.paths.erase(v->second);
        s.synthetic.erase(v);
      }
    }
  }
  bump_version(parent->ino);
  if (newparent != parent) bump_version(newparent->ino);
  return 0;
}

int f_ll_readlink(ceph_mount_info*, Inode* in, char* buf, size_t size, const UserPerm* perms) {
  note_identity(perms);
  FAKE_FAIL();
  ssize_t n = readlinkat(in->fd, "", buf, size);
  return n < 0 ? -errno : static_cast<int>(n);
}

int f_ll_statfs(ceph_mount_info*, Inode* in, struct statvfs* out) {
  FAKE_FAIL();
  return statvfs(proc_path(in->fd).c_str(), out) < 0 ? -errno : 0;
}

uint32_t f_ll_stripe_unit(ceph_mount_info*, Inode*) { return 4u << 20; }

// ---- file handles -------------------------------------------------------------------

void release_locks_of(Fh* fh) {
  auto& s = st();
  std::lock_guard lock(s.mu);
  std::erase_if(s.locks, [&](const State::Seg& seg) { return seg.fh == fh; });
}

int f_ll_read(ceph_mount_info*, Fh* fh, int64_t off, uint64_t len, char* buf) {
  FAKE_FAIL();
  ssize_t r = pread(fh->fd, buf, static_cast<size_t>(len), static_cast<off_t>(off));
  return r < 0 ? -errno : static_cast<int>(r);
}
int f_ll_write(ceph_mount_info*, Fh* fh, int64_t off, uint64_t len, const char* data) {
  FAKE_FAIL();
  ssize_t r = pwrite(fh->fd, data, static_cast<size_t>(len), static_cast<off_t>(off));
  if (r < 0) return -errno;
  bump_version(fh->ino);
  return static_cast<int>(r);
}
int64_t f_ll_writev(ceph_mount_info*, Fh* fh, const struct iovec* iov, int cnt, int64_t off) {
  FAKE_FAIL();
  ssize_t r = pwritev(fh->fd, iov, cnt, static_cast<off_t>(off));
  if (r < 0) return -errno;
  bump_version(fh->ino);
  return r;
}
int f_ll_fsync(ceph_mount_info*, Fh* fh, int dataonly) {
  FAKE_FAIL();
  int rc = dataonly ? fdatasync(fh->fd) : fsync(fh->fd);
  return rc < 0 ? -errno : 0;
}
off_t f_ll_lseek(ceph_mount_info*, Fh* fh, off_t off, int whence) {
  int e = 0;
  if (take_fail(e)) return -e;
  off_t r = lseek(fh->fd, off, whence);
  return r < 0 ? -errno : r;
}
int f_ll_fallocate(ceph_mount_info*, Fh* fh, int mode, int64_t off, int64_t len) {
  FAKE_FAIL();
  int flags = 0;
  if (mode & capi::kFallocKeepSize) flags |= FALLOC_FL_KEEP_SIZE;
  if (mode & capi::kFallocPunchHole) flags |= FALLOC_FL_PUNCH_HOLE;
  // Ceph accepts mode 0 and PUNCH_HOLE|KEEP_SIZE only.
  if (mode != 0 && mode != (capi::kFallocKeepSize | capi::kFallocPunchHole)) return -EOPNOTSUPP;
  if (fallocate(fh->fd, flags, static_cast<off_t>(off), static_cast<off_t>(len)) < 0) return -errno;
  bump_version(fh->ino);
  return 0;
}
int f_ll_close(ceph_mount_info*, Fh* fh) {
  if (!fh) return -EINVAL;
  release_locks_of(fh);
  if (fh->fd >= 0) close(fh->fd);
  st().live_fhs.fetch_sub(1);
  delete fh;
  return 0;
}

int do_lock(Fh* fh, struct flock* fl, uint64_t owner, bool test) {
  auto& s = st();
  std::lock_guard lock(s.mu);
  uint64_t start = static_cast<uint64_t>(fl->l_start);
  uint64_t end = fl->l_len == 0 ? UINT64_MAX : start + static_cast<uint64_t>(fl->l_len);
  auto overlaps = [&](const State::Seg& seg) {
    return seg.ino == fh->ino && seg.start < end && start < seg.end;
  };
  auto carve = [&](std::vector<State::Seg>& keep) {
    for (auto& seg : s.locks) {
      if (seg.ino != fh->ino || seg.owner != owner || !overlaps(seg)) {
        keep.push_back(seg);
        continue;
      }
      if (seg.start < start) keep.push_back({seg.ino, seg.owner, seg.fh, seg.start, start, seg.excl});
      if (seg.end > end) keep.push_back({seg.ino, seg.owner, seg.fh, end, seg.end, seg.excl});
    }
  };
  if (fl->l_type == F_UNLCK) {
    if (test) return 0;
    std::vector<State::Seg> keep;
    carve(keep);
    s.locks = std::move(keep);
    return 0;
  }
  bool excl = fl->l_type == F_WRLCK;
  for (const auto& seg : s.locks) {
    if (seg.owner == owner || !overlaps(seg)) continue;
    if (!(excl || seg.excl)) continue;
    if (test) {
      fl->l_type = seg.excl ? F_WRLCK : F_RDLCK;
      fl->l_start = static_cast<off_t>(seg.start);
      fl->l_len = seg.end == UINT64_MAX ? 0 : static_cast<off_t>(seg.end - seg.start);
      fl->l_pid = 0;
      return 0;
    }
    return -EAGAIN;
  }
  if (test) {
    fl->l_type = F_UNLCK;
    return 0;
  }
  std::vector<State::Seg> keep;
  carve(keep);
  keep.push_back({fh->ino, owner, fh, start, end, excl});
  s.locks = std::move(keep);
  return 0;
}

int f_ll_getlk(ceph_mount_info*, Fh* fh, struct flock* fl, uint64_t owner) {
  FAKE_FAIL();
  return do_lock(fh, fl, owner, true);
}
int f_ll_setlk(ceph_mount_info*, Fh* fh, struct flock* fl, uint64_t owner, int) {
  FAKE_FAIL();
  return do_lock(fh, fl, owner, false);
}

// ---- directories ----------------------------------------------------------------

int f_ll_opendir(ceph_mount_info* m, Inode* in, ceph_dir_result** out, const UserPerm* perms) {
  note_identity(perms);
  FAKE_FAIL();
  int rc = check_dir(in, R_OK, perms);
  if (rc < 0) return rc;
  int fd = open(proc_path(in->fd).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return -errno;
  DIR* d = fdopendir(fd);
  if (!d) {
    int e = errno;
    close(fd);
    return -e;
  }
  auto* dr = new ceph_dir_result;
  dr->mount = m;
  dr->dir = d;
  dr->dirfd = fd;
  dr->ino = in->ino;
  st().live_dirs.fetch_add(1);
  *out = dr;
  return 0;
}
int f_ll_releasedir(ceph_mount_info*, ceph_dir_result* dr) {
  if (!dr) return -EINVAL;
  closedir(dr->dir);
  st().live_dirs.fetch_sub(1);
  delete dr;
  return 0;
}
int f_readdirplus_r(ceph_mount_info* m, ceph_dir_result* dr, struct dirent* de,
                    struct ceph_statx* stx, unsigned, unsigned, Inode** out) {
  FAKE_FAIL();
  errno = 0;
  struct dirent* d = readdir(dr->dir);
  if (!d) return errno ? -errno : 0;
  *de = *d;
  struct stat sb {};
  if (fstatat(dr->dirfd, d->d_name, &sb, AT_SYMLINK_NOFOLLOW) < 0) return -errno;
  std::string name = d->d_name;
  std::string path = name == "." ? path_of(dr->ino)
                     : name == ".." ? join(path_of(dr->ino), "..")
                                    : join(path_of(dr->ino), name);
  uint64_t ino = synthetic_ino(sb, path, false);
  de->d_ino = ino;
  fill_statx(sb, ino, stx);
  if (out) {
    int fd = openat(dr->dirfd, d->d_name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    *out = make_inode(m, fd, path, false);
    if (!*out) return -EIO;
  }
  return 1;
}
void f_seekdir(ceph_mount_info*, ceph_dir_result* dr, int64_t off) { seekdir(dr->dir, off); }

}  // namespace

namespace lnfs::testing {

std::shared_ptr<const Api> FakeCephApi::api() {
  static std::shared_ptr<const Api> table = [] {
    auto a = std::make_shared<Api>();
    a->ceph_version = f_version;
    a->ceph_create = f_create;
    a->ceph_conf_read_file = f_conf_read_file;
    a->ceph_conf_set = f_conf_set;
    a->ceph_conf_get = f_conf_get;
    a->ceph_init = f_init;
    a->ceph_select_filesystem = f_select_filesystem;
    a->ceph_mount = f_mount;
    a->ceph_unmount = f_unmount;
    a->ceph_release = f_release;
    a->ceph_shutdown = f_shutdown;
    a->ceph_get_fs_cid = f_get_fs_cid;
    a->ceph_get_instance_id = f_get_instance_id;
    a->ceph_userperm_new = f_userperm_new;
    a->ceph_userperm_destroy = f_userperm_destroy;
    a->ceph_ll_lookup_root = f_ll_lookup_root;
    a->ceph_ll_lookup_vino = f_ll_lookup_vino;
    a->ceph_ll_lookup = f_ll_lookup;
    a->ceph_ll_put = f_ll_put;
    a->ceph_ll_getattr = f_ll_getattr;
    a->ceph_ll_setattr = f_ll_setattr;
    a->ceph_ll_open = f_ll_open;
    a->ceph_ll_create = f_ll_create;
    a->ceph_ll_mknod = f_ll_mknod;
    a->ceph_ll_mkdir = f_ll_mkdir;
    a->ceph_ll_symlink = f_ll_symlink;
    a->ceph_ll_link = f_ll_link;
    a->ceph_ll_unlink = f_ll_unlink;
    a->ceph_ll_rmdir = f_ll_rmdir;
    a->ceph_ll_rename = f_ll_rename;
    a->ceph_ll_readlink = f_ll_readlink;
    a->ceph_ll_statfs = f_ll_statfs;
    a->ceph_ll_stripe_unit = f_ll_stripe_unit;
    a->ceph_ll_read = f_ll_read;
    a->ceph_ll_write = f_ll_write;
    a->ceph_ll_writev = f_ll_writev;
    a->ceph_ll_fsync = f_ll_fsync;
    a->ceph_ll_lseek = f_ll_lseek;
    a->ceph_ll_fallocate = f_ll_fallocate;
    a->ceph_ll_close = f_ll_close;
    a->ceph_ll_getlk = f_ll_getlk;
    a->ceph_ll_setlk = f_ll_setlk;
    a->ceph_ll_opendir = f_ll_opendir;
    a->ceph_ll_releasedir = f_ll_releasedir;
    a->ceph_readdirplus_r = f_readdirplus_r;
    a->ceph_seekdir = f_seekdir;
    return std::shared_ptr<const Api>(a);
  }();
  return table;
}

void FakeCephApi::set_root(std::string dir) {
  auto& s = st();
  std::lock_guard lock(s.mu);
  s.root = std::move(dir);
  s.synthetic.clear();
  s.paths.clear();
  s.versions.clear();
  s.locks.clear();
  s.next_ino = 0x10000000000ull;
  s.fail_mount = 0;
  s.fail_left.store(0);
}
void FakeCephApi::fail_mount(int err) {
  auto& s = st();
  std::lock_guard lock(s.mu);
  s.fail_mount = err;
}
void FakeCephApi::fail_next(int err, int count) {
  st().fail_err.store(err);
  st().fail_left.store(count);
}
uint32_t FakeCephApi::last_uid() { return g_last_uid.load(); }
uint32_t FakeCephApi::last_gid() { return g_last_gid.load(); }
int FakeCephApi::live_inodes() { return st().live_inodes.load(); }
int FakeCephApi::live_fhs() { return st().live_fhs.load(); }
int FakeCephApi::live_dirs() { return st().live_dirs.load(); }
int FakeCephApi::live_perms() { return st().live_perms.load(); }
uint64_t FakeCephApi::getattr_calls() { return st().getattr_calls.load(); }

}  // namespace lnfs::testing
