#include "gfapi_fake.hpp"

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

// The opaque types are declared in the global namespace by backend/gfapi.hpp; the
// fake defines them.
struct glfs {
  std::string root;
  int root_fd = -1;
  bool inited = false;
  std::string volname;
};

struct glfs_object {
  glfs* fs = nullptr;
  int fd = -1;  // O_PATH
  uint64_t ino = 0;
  uint64_t gen = 0;
};

struct glfs_fd {
  glfs* fs = nullptr;
  int fd = -1;         // data fd (regular files)
  DIR* dir = nullptr;  // directories
  uint64_t ino = 0;
  std::string lkowner;
};

struct glfs_xreaddirp_stat {
  struct stat st {};
  glfs_object* obj = nullptr;
  bool has_stat = false;
};

namespace {

using lnfs::backend::gfapi::Api;

struct State {
  std::mutex mu;
  std::string root;
  int fail_init = 0;
  std::atomic<int> fail_left{0};
  std::atomic<int> fail_err{0};
  std::atomic<int> live_objects{0};
  std::atomic<int> live_fds{0};
  std::atomic<uint64_t> access_calls{0};
  // ino → generation: bumped on every creation so a re-created inode gets a new
  // handle (P2).  ino → relative path: the "inode table" behind create_from_handle.
  std::unordered_map<uint64_t, uint64_t> gens;
  std::unordered_map<uint64_t, std::string> paths;
  uint64_t next_gen = 1;
  // posix locks: (ino, owner) segments
  struct Seg {
    uint64_t ino;
    std::string owner;
    uint64_t start, end;  // end exclusive, UINT64_MAX = EOF
    bool excl;
  };
  std::vector<Seg> locks;
};

State& st() {
  static State s;
  return s;
}

thread_local uint32_t t_fsuid = 0;
thread_local uint32_t t_fsgid = 0;
thread_local std::vector<gid_t> t_groups;
std::atomic<uint32_t> g_last_uid{0}, g_last_gid{0};

void note_identity() {
  g_last_uid.store(t_fsuid, std::memory_order_relaxed);
  g_last_gid.store(t_fsgid, std::memory_order_relaxed);
}

// Injected failure for the next N fops.
bool take_fail() {
  auto& s = st();
  int cur = s.fail_left.load();
  while (cur > 0) {
    if (s.fail_left.compare_exchange_weak(cur, cur - 1)) {
      errno = s.fail_err.load();
      return true;
    }
  }
  return false;
}

std::string proc_path(int fd) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "/proc/self/fd/%d", fd);
  return buf;
}

std::string join(const std::string& parent, const std::string& name) {
  if (name == "." ) return parent;
  if (name == "..") {
    auto slash = parent.rfind('/');
    return slash == std::string::npos ? std::string(".") : parent.substr(0, slash);
  }
  return parent == "." ? name : parent + "/" + name;
}

// Materializes an object for an O_PATH fd (adopts fd).  `path` is remembered for
// create_from_handle.
glfs_object* make_object(glfs* fs, int fd, const std::string& path, bool created) {
  struct stat sb {};
  if (fstat(fd, &sb) < 0) {
    int e = errno;
    close(fd);
    errno = e;
    return nullptr;
  }
  auto& s = st();
  auto* o = new glfs_object;
  o->fs = fs;
  o->fd = fd;
  o->ino = sb.st_ino;
  {
    std::lock_guard lock(s.mu);
    if (created || !s.gens.contains(sb.st_ino)) s.gens[sb.st_ino] = s.next_gen++;
    o->gen = s.gens[sb.st_ino];
    s.paths[sb.st_ino] = path;
  }
  s.live_objects.fetch_add(1);
  return o;
}

std::string path_of(uint64_t ino) {
  auto& s = st();
  std::lock_guard lock(s.mu);
  auto it = s.paths.find(ino);
  return it == s.paths.end() ? std::string() : it->second;
}

int stat_object(glfs_object* o, struct stat* out) {
  return fstatat(o->fd, "", out, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
}

// ---- lifecycle ----------------------------------------------------------------

glfs* f_new(const char* volname) {
  auto* fs = new glfs;
  fs->volname = volname ? volname : "";
  return fs;
}
int f_set_volfile_server(glfs*, const char* transport, const char* host, int port) {
  if (!transport || !host || port <= 0) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}
int f_set_logging(glfs*, const char*, int) { return 0; }
int f_init(glfs* fs) {
  auto& s = st();
  std::string root;
  int fail = 0;
  {
    std::lock_guard lock(s.mu);
    root = s.root;
    fail = s.fail_init;
  }
  if (fail) {
    errno = fail;
    return -1;
  }
  int fd = open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return -1;
  fs->root = root;
  fs->root_fd = fd;
  fs->inited = true;
  return 0;
}
int f_fini(glfs* fs) {
  if (fs->root_fd >= 0) close(fs->root_fd);
  delete fs;
  return 0;
}
int f_get_volumeid(glfs*, char* volid, size_t size) {
  if (size < 16) {
    errno = ERANGE;
    return -1;
  }
  for (int i = 0; i < 16; ++i) volid[i] = static_cast<char>(0xA0 + i);
  return 16;
}
int f_setfsuid(uid_t uid) {
  t_fsuid = uid;
  return 0;
}
int f_setfsgid(gid_t gid) {
  t_fsgid = gid;
  return 0;
}
int f_setfsgroups(size_t n, const gid_t* list) {
  t_groups.assign(list, list + n);
  return 0;
}

// ---- handle-based namespace ops -------------------------------------------------

glfs_object* f_h_lookupat(glfs* fs, glfs_object* parent, const char* path, struct stat* out,
                          int follow) {
  note_identity();
  if (take_fail()) return nullptr;
  int base = parent ? parent->fd : fs->root_fd;
  std::string rel = parent ? path_of(parent->ino) : ".";
  std::string p = path;
  // Absolute (from the root) or relative to `parent`; components split on '/'.
  if (!p.empty() && p.front() == '/') {
    base = fs->root_fd;
    rel = ".";
    p.erase(0, 1);
  }
  if (p.empty()) p = ".";
  int fd = dup(base);
  size_t pos = 0;
  while (pos <= p.size()) {
    size_t slash = p.find('/', pos);
    std::string comp = p.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
    pos = slash == std::string::npos ? p.size() + 1 : slash + 1;
    if (comp.empty()) continue;
    int next = openat(fd, comp.c_str(), O_PATH | O_CLOEXEC | (follow ? 0 : O_NOFOLLOW));
    int e = errno;
    close(fd);
    if (next < 0) {
      errno = e;
      return nullptr;
    }
    fd = next;
    rel = join(rel, comp);
  }
  auto* o = make_object(fs, fd, rel, false);
  if (o && out) stat_object(o, out);
  return o;
}

glfs_object* f_h_creat(glfs* fs, glfs_object* parent, const char* name, int flags,
                       mode_t mode, struct stat* out) {
  note_identity();
  if (take_fail()) return nullptr;
  int fd = openat(parent->fd, name, flags | O_CLOEXEC | O_NOFOLLOW, mode);
  if (fd < 0) return nullptr;
  close(fd);
  int pfd = openat(parent->fd, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
  if (pfd < 0) return nullptr;
  auto* o = make_object(fs, pfd, join(path_of(parent->ino), name), true);
  if (o && out) stat_object(o, out);
  return o;
}

glfs_object* f_h_mkdir(glfs* fs, glfs_object* parent, const char* name, mode_t mode,
                       struct stat* out) {
  note_identity();
  if (take_fail()) return nullptr;
  if (mkdirat(parent->fd, name, mode) < 0) return nullptr;
  int pfd = openat(parent->fd, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
  if (pfd < 0) return nullptr;
  auto* o = make_object(fs, pfd, join(path_of(parent->ino), name), true);
  if (o && out) stat_object(o, out);
  return o;
}

glfs_object* f_h_mknod(glfs* fs, glfs_object* parent, const char* name, mode_t mode,
                       dev_t dev, struct stat* out) {
  note_identity();
  if (take_fail()) return nullptr;
  if (mknodat(parent->fd, name, mode, dev) < 0) return nullptr;
  int pfd = openat(parent->fd, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
  if (pfd < 0) return nullptr;
  auto* o = make_object(fs, pfd, join(path_of(parent->ino), name), true);
  if (o && out) stat_object(o, out);
  return o;
}

glfs_object* f_h_symlink(glfs* fs, glfs_object* parent, const char* name, const char* data,
                         struct stat* out) {
  note_identity();
  if (take_fail()) return nullptr;
  if (symlinkat(data, parent->fd, name) < 0) return nullptr;
  int pfd = openat(parent->fd, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
  if (pfd < 0) return nullptr;
  auto* o = make_object(fs, pfd, join(path_of(parent->ino), name), true);
  if (o && out) stat_object(o, out);
  return o;
}

int f_h_unlink(glfs*, glfs_object* parent, const char* name) {
  note_identity();
  if (take_fail()) return -1;
  struct stat sb {};
  if (fstatat(parent->fd, name, &sb, AT_SYMLINK_NOFOLLOW) < 0) return -1;
  return unlinkat(parent->fd, name, S_ISDIR(sb.st_mode) ? AT_REMOVEDIR : 0);
}

int f_h_close(glfs_object* o) {
  if (!o) return -1;
  if (o->fd >= 0) close(o->fd);
  st().live_objects.fetch_sub(1);
  delete o;
  return 0;
}

int f_h_truncate(glfs*, glfs_object* o, off_t len) {
  note_identity();
  if (take_fail()) return -1;
  int fd = open(proc_path(o->fd).c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  int rc = ftruncate(fd, len);
  int e = errno;
  close(fd);
  errno = e;
  return rc;
}

int f_h_stat(glfs*, glfs_object* o, struct stat* out) {
  if (take_fail()) return -1;
  return stat_object(o, out);
}

int f_h_statfs(glfs*, glfs_object* o, struct statvfs* out) {
  if (take_fail()) return -1;
  return statvfs(proc_path(o->fd).c_str(), out);
}

int f_h_setattrs(glfs*, glfs_object* o, struct stat* sb, int valid) {
  note_identity();
  if (take_fail()) return -1;
  using namespace lnfs::backend::gfapi;
  std::string p = proc_path(o->fd);
  if (valid & kSetSize) {
    int fd = open(p.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0 || ftruncate(fd, sb->st_size) < 0) {
      int e = errno;
      if (fd >= 0) close(fd);
      errno = e;
      return -1;
    }
    close(fd);
  }
  if ((valid & kSetMode) && chmod(p.c_str(), sb->st_mode & 07777) < 0) return -1;
  if (valid & (kSetUid | kSetGid)) {
    uid_t uid = (valid & kSetUid) ? sb->st_uid : static_cast<uid_t>(-1);
    gid_t gid = (valid & kSetGid) ? sb->st_gid : static_cast<gid_t>(-1);
    if (fchownat(o->fd, "", uid, gid, AT_EMPTY_PATH) < 0) return -1;
  }
  if (valid & (kSetAtime | kSetMtime)) {
    timespec times[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};
    if (valid & kSetAtime) times[0] = sb->st_atim;
    if (valid & kSetMtime) times[1] = sb->st_mtim;
    if (utimensat(AT_FDCWD, p.c_str(), times, 0) < 0) return -1;
  }
  return 0;
}

int f_h_readlink(glfs*, glfs_object* o, char* buf, size_t size) {
  if (take_fail()) return -1;
  ssize_t n = readlinkat(o->fd, "", buf, size);
  return n < 0 ? -1 : static_cast<int>(n);
}

int f_h_link(glfs*, glfs_object* target, glfs_object* parent, const char* name) {
  note_identity();
  if (take_fail()) return -1;
  if (linkat(AT_FDCWD, proc_path(target->fd).c_str(), parent->fd, name, AT_SYMLINK_FOLLOW) < 0)
    return -1;
  return 0;
}

int f_h_rename(glfs*, glfs_object* olddir, const char* oldname, glfs_object* newdir,
               const char* newname) {
  note_identity();
  if (take_fail()) return -1;
  if (renameat(olddir->fd, oldname, newdir->fd, newname) < 0) return -1;
  // keep the inode table pointing at the new name
  struct stat sb {};
  if (fstatat(newdir->fd, newname, &sb, AT_SYMLINK_NOFOLLOW) == 0) {
    std::string moved = join(path_of(newdir->ino), newname);  // path_of takes the lock
    auto& s = st();
    std::lock_guard lock(s.mu);
    s.paths[sb.st_ino] = moved;
  }
  return 0;
}

ssize_t f_h_extract_handle(glfs_object* o, unsigned char* handle, int len) {
  if (len < 16) {
    errno = ERANGE;
    return -1;
  }
  std::memcpy(handle, &o->ino, 8);
  std::memcpy(handle + 8, &o->gen, 8);
  return 16;
}

glfs_object* f_h_create_from_handle(glfs* fs, unsigned char* handle, int len, struct stat* out) {
  if (take_fail()) return nullptr;
  if (len != 16) {
    errno = EINVAL;
    return nullptr;
  }
  uint64_t ino = 0, gen = 0;
  std::memcpy(&ino, handle, 8);
  std::memcpy(&gen, handle + 8, 8);
  std::string rel;
  uint64_t cur_gen = 0;
  {
    auto& s = st();
    std::lock_guard lock(s.mu);
    auto it = s.paths.find(ino);
    if (it != s.paths.end()) rel = it->second;
    auto g = s.gens.find(ino);
    if (g != s.gens.end()) cur_gen = g->second;
  }
  if (rel.empty() || cur_gen != gen) {
    errno = ENOENT;
    return nullptr;
  }
  int fd = openat(fs->root_fd, rel.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    errno = ENOENT;
    return nullptr;
  }
  struct stat sb {};
  if (fstat(fd, &sb) < 0 || sb.st_ino != ino) {
    close(fd);
    errno = ENOENT;
    return nullptr;
  }
  auto* o = make_object(fs, fd, rel, false);
  if (o && out) *out = sb;
  return o;
}

glfs_fd* f_h_opendir(glfs* fs, glfs_object* o) {
  note_identity();
  if (take_fail()) return nullptr;
  int fd = open(proc_path(o->fd).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return nullptr;
  DIR* d = fdopendir(fd);
  if (!d) {
    close(fd);
    return nullptr;
  }
  auto* g = new glfs_fd;
  g->fs = fs;
  g->dir = d;
  g->ino = o->ino;
  st().live_fds.fetch_add(1);
  return g;
}

// Permission model of the bricks: mode bits under the thread's fsuid/fsgid/groups.
bool permits(const struct stat& sb, int mode) {
  if (t_fsuid == 0) return true;
  int shift = 0;
  if (t_fsuid == sb.st_uid) shift = 6;
  else if (t_fsgid == sb.st_gid ||
           std::find(t_groups.begin(), t_groups.end(), sb.st_gid) != t_groups.end())
    shift = 3;
  int bits = (sb.st_mode >> shift) & 7;
  if ((mode & R_OK) && !(bits & 4)) return false;
  if ((mode & W_OK) && !(bits & 2)) return false;
  if ((mode & X_OK) && !(bits & 1)) return false;
  return true;
}

glfs_fd* f_h_open(glfs* fs, glfs_object* o, int flags) {
  note_identity();
  if (take_fail()) return nullptr;
  struct stat sb {};
  if (stat_object(o, &sb) < 0) return nullptr;
  int acc = flags & O_ACCMODE;
  int need = acc == O_RDONLY ? R_OK : acc == O_WRONLY ? W_OK : R_OK | W_OK;
  if (!permits(sb, need)) {
    errno = EACCES;
    return nullptr;
  }
  int fd = open(proc_path(o->fd).c_str(), flags | O_CLOEXEC);
  if (fd < 0 && errno == EACCES && t_fsuid == 0) {
    // The bricks run as root and honour fsuid 0 regardless of mode bits; this test
    // process does not, so emulate that for the gateway identity by briefly widening
    // the mode (tests only).
    std::string p = proc_path(o->fd);
    if (chmod(p.c_str(), (sb.st_mode & 07777) | 0600) == 0) {
      fd = open(p.c_str(), flags | O_CLOEXEC);
      int e = errno;
      (void)chmod(p.c_str(), sb.st_mode & 07777);
      errno = e;
    }
  }
  if (fd < 0) return nullptr;
  auto* g = new glfs_fd;
  g->fs = fs;
  g->fd = fd;
  g->ino = o->ino;
  st().live_fds.fetch_add(1);
  return g;
}

int f_h_access(glfs*, glfs_object* o, int mask) {
  note_identity();
  st().access_calls.fetch_add(1);
  if (take_fail()) return -1;
  struct stat sb {};
  if (stat_object(o, &sb) < 0) return -1;
  if (!permits(sb, mask)) {
    errno = EACCES;
    return -1;
  }
  return 0;
}

// ---- fd ops -------------------------------------------------------------------

void release_locks_of(glfs_fd* g) {
  if (g->lkowner.empty()) return;
  auto& s = st();
  std::lock_guard lock(s.mu);
  std::erase_if(s.locks, [&](const State::Seg& seg) {
    return seg.ino == g->ino && seg.owner == g->lkowner;
  });
}

int f_close(glfs_fd* g) {
  if (!g) return -1;
  release_locks_of(g);
  if (g->fd >= 0) close(g->fd);
  st().live_fds.fetch_sub(1);
  delete g;
  return 0;
}
int f_closedir(glfs_fd* g) {
  if (!g) return -1;
  if (g->dir) closedir(g->dir);
  st().live_fds.fetch_sub(1);
  delete g;
  return 0;
}
int f_fstat(glfs_fd* g, struct stat* out) {
  if (take_fail()) return -1;
  return fstat(g->fd, out);
}
ssize_t f_pread(glfs_fd* g, void* buf, size_t n, off_t off, int, glfs_stat*) {
  if (take_fail()) return -1;
  return pread(g->fd, buf, n, off);
}
ssize_t f_pwrite(glfs_fd* g, const void* buf, size_t n, off_t off, int, glfs_stat*, glfs_stat*) {
  if (take_fail()) return -1;
  return pwrite(g->fd, buf, n, off);
}
ssize_t f_pwritev(glfs_fd* g, const struct iovec* iov, int cnt, off_t off, int) {
  if (take_fail()) return -1;
  return pwritev(g->fd, iov, cnt, off);
}
int f_fsync(glfs_fd* g, glfs_stat*, glfs_stat*) {
  if (take_fail()) return -1;
  return fsync(g->fd);
}
int f_fdatasync(glfs_fd* g, glfs_stat*, glfs_stat*) {
  if (take_fail()) return -1;
  return fdatasync(g->fd);
}
off_t f_lseek(glfs_fd* g, off_t off, int whence) {
  if (take_fail()) return -1;
  return lseek(g->fd, off, whence);
}
int f_fallocate(glfs_fd* g, int keep_size, off_t off, size_t len) {
  if (take_fail()) return -1;
  return fallocate(g->fd, keep_size ? FALLOC_FL_KEEP_SIZE : 0, off, static_cast<off_t>(len));
}
int f_discard(glfs_fd* g, off_t off, size_t len) {
  if (take_fail()) return -1;
  return fallocate(g->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off,
                   static_cast<off_t>(len));
}
ssize_t f_copy_file_range(glfs_fd* in, off64_t* off_in, glfs_fd* out, off64_t* off_out,
                          size_t len, unsigned int flags, glfs_stat*, glfs_stat*, glfs_stat*) {
  if (take_fail()) return -1;
  return copy_file_range(in->fd, off_in, out->fd, off_out, len, flags);
}

// ---- directories ----------------------------------------------------------------

int f_xreaddirplus_r(glfs_fd* g, uint32_t flags, glfs_xreaddirp_stat** xstat_p,
                     struct dirent* ext, struct dirent** res) {
  using namespace lnfs::backend::gfapi;
  if (take_fail()) return -1;
  *xstat_p = nullptr;
  errno = 0;
  struct dirent* d = readdir(g->dir);
  if (!d) {
    *res = nullptr;
    return errno ? -1 : 0;
  }
  *ext = *d;
  *res = ext;
  int done = 0;
  if (flags) {
    auto* x = new glfs_xreaddirp_stat;
    int dfd = dirfd(g->dir);
    if ((flags & kXreaddirpStat) &&
        fstatat(dfd, d->d_name, &x->st, AT_SYMLINK_NOFOLLOW) == 0) {
      x->has_stat = true;
      done |= kXreaddirpStat;
    }
    if (flags & kXreaddirpHandle) {
      int pfd = openat(dfd, d->d_name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
      if (pfd >= 0) {
        x->obj = make_object(g->fs, pfd, join(path_of(g->ino), d->d_name), false);
        if (x->obj) done |= kXreaddirpHandle;
      }
    }
    *xstat_p = x;
  }
  return done;
}
struct stat* f_xreaddirplus_get_stat(glfs_xreaddirp_stat* x) {
  return x && x->has_stat ? &x->st : nullptr;
}
glfs_object* f_xreaddirplus_get_object(glfs_xreaddirp_stat* x) { return x ? x->obj : nullptr; }
void f_seekdir(glfs_fd* g, long off) { seekdir(g->dir, off); }
void f_free(void* p) {
  auto* x = static_cast<glfs_xreaddirp_stat*>(p);
  if (!x) return;
  if (x->obj) f_h_close(x->obj);
  delete x;
}

// ---- posix locks -----------------------------------------------------------------

int f_fd_set_lkowner(glfs_fd* g, void* data, int len) {
  if (len <= 0 || len > 255) {
    errno = EINVAL;
    return -1;
  }
  g->lkowner.assign(static_cast<const char*>(data), static_cast<size_t>(len));
  return 0;
}

int f_posix_lock(glfs_fd* g, int cmd, struct flock* fl) {
  if (take_fail()) return -1;
  if (g->lkowner.empty()) g->lkowner = "fd:" + std::to_string(reinterpret_cast<uintptr_t>(g));
  auto& s = st();
  std::lock_guard lock(s.mu);
  uint64_t start = static_cast<uint64_t>(fl->l_start);
  uint64_t end = fl->l_len == 0 ? UINT64_MAX : start + static_cast<uint64_t>(fl->l_len);
  auto overlaps = [&](const State::Seg& seg) {
    return seg.ino == g->ino && seg.start < end && start < seg.end;
  };
  if (fl->l_type == F_UNLCK) {
    if (cmd == F_GETLK) return 0;
    std::vector<State::Seg> keep;
    for (auto& seg : s.locks) {
      if (seg.ino != g->ino || seg.owner != g->lkowner || !overlaps(seg)) {
        keep.push_back(seg);
        continue;
      }
      if (seg.start < start) keep.push_back({seg.ino, seg.owner, seg.start, start, seg.excl});
      if (seg.end > end) keep.push_back({seg.ino, seg.owner, end, seg.end, seg.excl});
    }
    s.locks = std::move(keep);
    return 0;
  }
  bool excl = fl->l_type == F_WRLCK;
  for (const auto& seg : s.locks) {
    if (seg.owner == g->lkowner || !overlaps(seg)) continue;
    if (!(excl || seg.excl)) continue;
    if (cmd == F_GETLK) {
      fl->l_type = seg.excl ? F_WRLCK : F_RDLCK;
      fl->l_start = static_cast<off_t>(seg.start);
      fl->l_len = seg.end == UINT64_MAX ? 0 : static_cast<off_t>(seg.end - seg.start);
      fl->l_pid = 0;
      return 0;
    }
    errno = EAGAIN;
    return -1;
  }
  if (cmd == F_GETLK) {
    fl->l_type = F_UNLCK;
    return 0;
  }
  // grant: replace this owner's coverage of the range, then add
  std::vector<State::Seg> keep;
  for (auto& seg : s.locks) {
    if (seg.ino != g->ino || seg.owner != g->lkowner || !overlaps(seg)) {
      keep.push_back(seg);
      continue;
    }
    if (seg.start < start) keep.push_back({seg.ino, seg.owner, seg.start, start, seg.excl});
    if (seg.end > end) keep.push_back({seg.ino, seg.owner, end, seg.end, seg.excl});
  }
  keep.push_back({g->ino, g->lkowner, start, end, excl});
  s.locks = std::move(keep);
  return 0;
}

}  // namespace

namespace lnfs::testing {

std::shared_ptr<const Api> FakeGfapi::api() {
  static std::shared_ptr<const Api> table = [] {
    auto a = std::make_shared<Api>();
    a->glfs_new = f_new;
    a->glfs_set_volfile_server = f_set_volfile_server;
    a->glfs_set_logging = f_set_logging;
    a->glfs_init = f_init;
    a->glfs_fini = f_fini;
    a->glfs_get_volumeid = f_get_volumeid;
    a->glfs_setfsuid = f_setfsuid;
    a->glfs_setfsgid = f_setfsgid;
    a->glfs_setfsgroups = f_setfsgroups;
    a->glfs_h_lookupat = f_h_lookupat;
    a->glfs_h_creat = f_h_creat;
    a->glfs_h_mkdir = f_h_mkdir;
    a->glfs_h_mknod = f_h_mknod;
    a->glfs_h_symlink = f_h_symlink;
    a->glfs_h_unlink = f_h_unlink;
    a->glfs_h_close = f_h_close;
    a->glfs_h_truncate = f_h_truncate;
    a->glfs_h_stat = f_h_stat;
    a->glfs_h_statfs = f_h_statfs;
    a->glfs_h_setattrs = f_h_setattrs;
    a->glfs_h_readlink = f_h_readlink;
    a->glfs_h_link = f_h_link;
    a->glfs_h_rename = f_h_rename;
    a->glfs_h_extract_handle = f_h_extract_handle;
    a->glfs_h_create_from_handle = f_h_create_from_handle;
    a->glfs_h_opendir = f_h_opendir;
    a->glfs_h_open = f_h_open;
    a->glfs_h_access = f_h_access;
    a->glfs_close = f_close;
    a->glfs_closedir = f_closedir;
    a->glfs_fstat = f_fstat;
    a->glfs_pread = f_pread;
    a->glfs_pwrite = f_pwrite;
    a->glfs_pwritev = f_pwritev;
    a->glfs_fsync = f_fsync;
    a->glfs_fdatasync = f_fdatasync;
    a->glfs_lseek = f_lseek;
    a->glfs_fallocate = f_fallocate;
    a->glfs_discard = f_discard;
    a->glfs_copy_file_range = f_copy_file_range;
    a->glfs_xreaddirplus_r = f_xreaddirplus_r;
    a->glfs_xreaddirplus_get_stat = f_xreaddirplus_get_stat;
    a->glfs_xreaddirplus_get_object = f_xreaddirplus_get_object;
    a->glfs_seekdir = f_seekdir;
    a->glfs_free = f_free;
    a->glfs_posix_lock = f_posix_lock;
    a->glfs_fd_set_lkowner = f_fd_set_lkowner;
    return std::shared_ptr<const Api>(a);
  }();
  return table;
}

void FakeGfapi::set_root(std::string dir) {
  auto& s = st();
  std::lock_guard lock(s.mu);
  s.root = std::move(dir);
  s.gens.clear();
  s.paths.clear();
  s.locks.clear();
  s.next_gen = 1;
  s.fail_init = 0;
  s.fail_left.store(0);
}
void FakeGfapi::fail_init(int err) {
  auto& s = st();
  std::lock_guard lock(s.mu);
  s.fail_init = err;
}
void FakeGfapi::fail_next(int err, int count) {
  st().fail_err.store(err);
  st().fail_left.store(count);
}
uint32_t FakeGfapi::last_fsuid() { return g_last_uid.load(); }
uint32_t FakeGfapi::last_fsgid() { return g_last_gid.load(); }
int FakeGfapi::live_objects() { return st().live_objects.load(); }
int FakeGfapi::live_fds() { return st().live_fds.load(); }
uint64_t FakeGfapi::access_calls() { return st().access_calls.load(); }

}  // namespace lnfs::testing
