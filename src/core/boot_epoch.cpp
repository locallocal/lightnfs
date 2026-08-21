#include "core/boot_epoch.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace lnfs::core {

Result<uint64_t> bump_boot_epoch(const std::string& state_dir) {
  std::string path = state_dir + "/boot_epoch";
  uint64_t epoch = 0;
  if (FILE* f = std::fopen(path.c_str(), "r")) {
    if (std::fscanf(f, "%" SCNu64, &epoch) != 1) epoch = 0;
    std::fclose(f);
  }
  ++epoch;

  std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return Err(errno_from(errno));
  char buf[32];
  int n = std::snprintf(buf, sizeof buf, "%" PRIu64 "\n", epoch);
  if (::write(fd, buf, static_cast<size_t>(n)) != n || ::fsync(fd) < 0) {
    int e = errno;
    ::close(fd);
    ::unlink(tmp.c_str());
    return Err(errno_from(e));
  }
  ::close(fd);
  if (::rename(tmp.c_str(), path.c_str()) < 0) {
    int e = errno;
    ::unlink(tmp.c_str());
    return Err(errno_from(e));
  }
  // Persist the rename itself so a crash directly after startup still sees the new epoch.
  int dir = ::open(state_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir >= 0) {
    (void)::fsync(dir);
    ::close(dir);
  }
  return epoch;
}

WriteVerf verifier_from_epoch(uint64_t epoch) {
  WriteVerf out{};
  std::memcpy(out.data(), &epoch, sizeof(epoch));
  return out;
}

}  // namespace lnfs::core
