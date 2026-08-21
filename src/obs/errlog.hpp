#pragma once
// Error-reply ring sampling (design 08 §8.2): with debug logging off, the most recent
// non-OK replies stay inspectable via `lightnfs-ctl dump-errors` — production triage
// without full debug output.

#include <cstdint>
#include <string>

namespace lnfs::obs {

void record_error_reply(std::string_view peer, uint32_t proc, uint32_t xid,
                        uint32_t status);
std::string dump_error_replies();

}  // namespace lnfs::obs
