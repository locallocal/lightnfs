#pragma once
// Error-reply ring sampling (design 08 §8.2): with debug logging off, the most recent
// non-OK replies stay inspectable via `lightnfs-ctl dump-errors` — production triage
// without full debug output.

#include <cstddef>
#include <cstdint>
#include <string>

namespace lnfs::obs {

// `what` names the failing procedure/operation (e.g. "GETATTR", "OPEN"); the caller
// resolves it so both the v3 and v4 engines can share the ring.
void record_error_reply(std::string_view peer, std::string_view what, uint32_t xid,
                        uint32_t status);
std::string dump_error_replies();

// Ring capacity ([server] error_ring, plan doc 10 §3.7; default 64). Resizing drops the
// entries recorded so far; call it at startup, before traffic.
void set_error_ring_capacity(size_t entries);

}  // namespace lnfs::obs
