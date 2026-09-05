#pragma once
// Durable whole-file replacement (design 07 §7.5, plan 10 A2): the "temp + write +
// fsync + rename + fsync(dir)" sequence the boot epoch always used, shared with the
// cluster store so every stable-state file is replaced atomically and survives a
// crash right after the write.  Blocking syscalls: call from the main thread or an
// offload thread, never on a reactor.

#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

#include "util/result.hpp"

namespace lnfs::core {

// Replaces `path` with `bytes` (the temp file is `<path>.tmp.<pid>.<n>`, unique per
// call; a failure leaves the old content untouched).  `mode` applies to a newly
// created file.
Result<void> atomic_write_file(const std::string& path, std::string_view bytes,
                               mode_t mode = 0600);

// `<path>.tmp.<pid>.<n>`: a temp name no other thread or process is using.
std::string unique_temp_name(const std::string& path);

// Whole-file read; nullopt when the file does not exist (a partner's write may not
// have landed yet), an error for anything else.
Result<std::optional<std::string>> read_file_if_exists(const std::string& path);

}  // namespace lnfs::core
