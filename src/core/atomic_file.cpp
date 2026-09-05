#include "core/atomic_file.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>

namespace lnfs::core {

std::string unique_temp_name(const std::string& path) {
  static std::atomic<uint64_t> counter{0};
  return path + ".tmp." + std::to_string(::getpid()) + "." +
         std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

Result<void> atomic_write_file(const std::string& path, std::string_view bytes, mode_t mode) {
  std::string tmp = unique_temp_name(path);
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) return Err(errno_from(errno));
  size_t done = 0;
  while (done < bytes.size()) {
    ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      ::close(fd);
      ::unlink(tmp.c_str());
      return Err(errno_from(e));
    }
    done += static_cast<size_t>(n);
  }
  if (::fsync(fd) < 0) {
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
  // Persist the rename itself so a crash directly afterwards still sees the new file.
  size_t slash = path.rfind('/');
  std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
  int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    (void)::fsync(dfd);
    ::close(dfd);
  }
  return {};
}

Result<std::optional<std::string>> read_file_if_exists(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) return std::optional<std::string>{};
    return Err(errno_from(errno));
  }
  std::string out;
  char buf[4096];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      ::close(fd);
      return Err(errno_from(e));
    }
    if (n == 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  return std::optional<std::string>{std::move(out)};
}

}  // namespace lnfs::core
