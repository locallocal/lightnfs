#pragma once
// Fault injection for the local backend data path (plan doc 10 §7.1).  Two drivers share
// the same per-kind budgets:
//   * environment — scripts/fault_inject.sh starts the server with LNFS_FAULT_*=N and the
//     first N matching operations fail (read once, at first use);
//   * programmatic — unit tests call arm()/clear(), which override any env leftover, so a
//     single test process can arm, exhaust and re-arm faults deterministically.
// take() consumes one budget unit; an unarmed kind costs one relaxed load.

#include <cstdint>

namespace lnfs::backend::fault {

enum class Kind : int {
  kFsyncEio = 0,  // fsync/fdatasync fails EIO           (LNFS_FAULT_FSYNC_EIO=N)
  kWriteEnospc,   // data write fails ENOSPC             (LNFS_FAULT_WRITE_ENOSPC=N)
  kWriteEdquot,   // data write fails EDQUOT             (LNFS_FAULT_WRITE_EDQUOT=N)
  kReadEio,       // data read fails EIO                 (LNFS_FAULT_READ_EIO=N)
  kShortWrite,    // data write completes only 1 byte    (LNFS_FAULT_SHORT_WRITE=N)
  kSlowIo,        // data op sleeps slow_ms() first      (LNFS_FAULT_SLOW_IO=N)
  kCount,
};

// Consume one budget unit of `k`; false when not armed.
bool take(Kind k);
// Set the remaining budget for `k` (tests; also re-arms after env budget is spent).
void arm(Kind k, int count);
// Disarm every kind (tests run in one process; clear() between cases).
void clear();

// Sleep applied by kSlowIo, milliseconds (default 50; LNFS_FAULT_SLOW_MS overrides).
int slow_ms();
void set_slow_ms(int ms);

}  // namespace lnfs::backend::fault
