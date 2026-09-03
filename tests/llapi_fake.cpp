#include "llapi_fake.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace lnfs::testing {
namespace {

using backend::llapi::Fid;
using backend::llapi::HsmState;

struct FidHash {
  size_t operator()(const Fid& f) const noexcept {
    return std::hash<uint64_t>{}(f.seq) ^ (std::hash<uint64_t>{}(f.oid) << 1) ^
           (std::hash<uint64_t>{}(f.ver) << 2);
  }
};

struct HsmEntry {
  uint32_t states = 0;
  bool restoring = false;
};

struct State {
  std::mutex mu;
  std::unordered_map<Fid, int, FidHash> pins;  // FID → O_PATH fd on the inode
  std::unordered_map<Fid, HsmEntry, FidHash> hsm;
  std::vector<Fid> restores;
  bool auto_restore = false;
  bool hsm_supported = true;
  uint32_t stripe = 0;
  bool lustre = true;
};

State& state() {
  static State s;
  return s;
}

std::string proc_fd_path(int fd) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "/proc/self/fd/%d", fd);
  return buf;
}

// The FID is a pure function of the inode identity: sequence fixed, oid = inode
// number, version = birth-time hash (so a recreated inode gets a different FID, P2).
Result<Fid> derive_fid(int fd) {
  struct statx sx {};
  if (::statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS | STATX_BTIME,
              &sx) < 0)
    return Err(errno_from(errno));
  Fid fid;
  fid.seq = 0x200000401ull;
  fid.oid = static_cast<uint32_t>(sx.stx_ino);
  uint64_t gen = 0;
  if (sx.stx_mask & STATX_BTIME)
    gen = static_cast<uint64_t>(sx.stx_btime.tv_sec) * 1000000007ull + sx.stx_btime.tv_nsec;
  fid.ver = static_cast<uint32_t>(sx.stx_ino >> 32) ^ static_cast<uint32_t>(gen ^ (gen >> 32));
  return fid;
}

bool inode_alive(int pin, struct stat* out = nullptr) {
  struct stat st {};
  if (::fstat(pin, &st) < 0) return false;
  if (out) *out = st;
  return st.st_nlink > 0;
}

class FakeOps final : public backend::llapi::Ops {
 public:
  bool is_lustre(int) const override {
    std::lock_guard lock(state().mu);
    return state().lustre;
  }

  Result<Fid> fid_of(int fd) const override {
    auto fid = derive_fid(fd);
    if (!fid) return fid;
    auto& s = state();
    std::lock_guard lock(s.mu);
    auto it = s.pins.find(*fid);
    if (it != s.pins.end()) {
      struct stat pinned {}, now {};
      if (inode_alive(it->second, &pinned) && ::fstat(fd, &now) == 0 &&
          pinned.st_dev == now.st_dev && pinned.st_ino == now.st_ino)
        return *fid;
      ::close(it->second);  // stale pin (inode recycled): re-pin below
      s.pins.erase(it);
    }
    int pin = ::open(proc_fd_path(fd).c_str(), O_PATH | O_CLOEXEC);
    if (pin < 0) return Err(errno_from(errno));
    s.pins.emplace(*fid, pin);
    return *fid;
  }

  Result<int> open_by_fid(int, const Fid& fid, int flags) const override {
    int pin = -1;
    {
      auto& s = state();
      std::lock_guard lock(s.mu);
      auto it = s.pins.find(fid);
      if (it == s.pins.end()) return Err(errno_from(ENOENT));
      pin = it->second;
    }
    if (!inode_alive(pin)) return Err(errno_from(ENOENT));
    // /proc's magic link lands on the pinned inode itself (a symlink inode included),
    // so O_NOFOLLOW must not be applied to the magic link.
    int fd = ::open(proc_fd_path(pin).c_str(), (flags & ~O_NOFOLLOW) | O_CLOEXEC);
    if (fd < 0) return Err(errno_from(errno));
    return fd;
  }

  Result<HsmState> hsm_state(int fd) const override {
    auto fid = derive_fid(fd);
    if (!fid) return Err(fid.error());
    auto& s = state();
    std::lock_guard lock(s.mu);
    if (!s.hsm_supported) return Err(errno_from(ENOTTY));
    HsmState out;
    auto it = s.hsm.find(*fid);
    if (it == s.hsm.end()) return out;
    out.states = it->second.states;
    out.archive_id = 1;
    if (it->second.restoring) {
      out.in_progress_state = backend::llapi::kHpsWaiting;
      out.in_progress_action = backend::llapi::kHuaRestore;
    }
    return out;
  }

  Result<void> hsm_restore(int, const Fid& fid) const override {
    auto& s = state();
    std::lock_guard lock(s.mu);
    s.restores.push_back(fid);
    auto& e = s.hsm[fid];
    if (s.auto_restore) {
      e.states &= ~backend::llapi::kHsReleased;
      e.restoring = false;
    } else {
      e.restoring = true;
    }
    return {};
  }

  Result<uint32_t> stripe_size(int) const override {
    std::lock_guard lock(state().mu);
    if (state().stripe == 0) return Err(errno_from(ENODATA));
    return state().stripe;
  }
};

}  // namespace

const backend::llapi::Ops* FakeLlapi::ops() {
  static const FakeOps value;
  return &value;
}

void FakeLlapi::reset() {
  auto& s = state();
  std::lock_guard lock(s.mu);
  for (auto& [fid, fd] : s.pins) ::close(fd);
  s.pins.clear();
  s.hsm.clear();
  s.restores.clear();
  s.auto_restore = false;
  s.hsm_supported = true;
  s.stripe = 0;
  s.lustre = true;
}

Fid FakeLlapi::fid_of_path(const std::string& path) {
  int fd = ::open(path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return {};
  auto fid = ops()->fid_of(fd);
  ::close(fd);
  return fid ? *fid : Fid{};
}

void FakeLlapi::set_hsm_states(const Fid& fid, uint32_t states) {
  auto& s = state();
  std::lock_guard lock(s.mu);
  auto& e = s.hsm[fid];
  e.states = states;
  if (!(states & backend::llapi::kHsReleased)) e.restoring = false;
}

uint32_t FakeLlapi::hsm_states(const Fid& fid) {
  auto& s = state();
  std::lock_guard lock(s.mu);
  auto it = s.hsm.find(fid);
  return it == s.hsm.end() ? 0 : it->second.states;
}

std::vector<Fid> FakeLlapi::restore_requests() {
  std::lock_guard lock(state().mu);
  return state().restores;
}

void FakeLlapi::set_auto_restore(bool on) {
  std::lock_guard lock(state().mu);
  state().auto_restore = on;
}

void FakeLlapi::set_hsm_supported(bool on) {
  std::lock_guard lock(state().mu);
  state().hsm_supported = on;
}

void FakeLlapi::set_stripe_size(uint32_t bytes) {
  std::lock_guard lock(state().mu);
  state().stripe = bytes;
}

void FakeLlapi::set_lustre(bool on) {
  std::lock_guard lock(state().mu);
  state().lustre = on;
}

int FakeLlapi::pinned() {
  std::lock_guard lock(state().mu);
  return static_cast<int>(state().pins.size());
}

}  // namespace lnfs::testing
