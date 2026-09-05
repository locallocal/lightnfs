#include "core/boot_epoch.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "core/atomic_file.hpp"

namespace lnfs::core {

Result<uint64_t> bump_boot_epoch(const std::string& state_dir) {
  std::string path = state_dir + "/boot_epoch";
  uint64_t epoch = 0;
  if (FILE* f = std::fopen(path.c_str(), "r")) {
    if (std::fscanf(f, "%" SCNu64, &epoch) != 1) epoch = 0;
    std::fclose(f);
  }
  ++epoch;
  char buf[32];
  int n = std::snprintf(buf, sizeof buf, "%" PRIu64 "\n", epoch);
  LNFS_TRY(atomic_write_file(path, std::string_view(buf, static_cast<size_t>(n))));
  return epoch;
}

WriteVerf verifier_from_epoch(uint64_t epoch) {
  WriteVerf out{};
  std::memcpy(out.data(), &epoch, sizeof(epoch));
  return out;
}

}  // namespace lnfs::core
