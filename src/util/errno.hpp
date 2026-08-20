#pragma once
// Strongly typed POSIX errno plus lightnfs-internal sentinels (design 05 §5.1, 04 §4.6).
// Backends and the runtime speak Errno; protocol engines map it to nfsstat at their boundary.

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

namespace lnfs {

enum class Errno : int32_t {
  kOk = 0,
  // Sentinels outside the POSIX range:
  kJukebox = 3000,  // backend HSM "try again later" -> v3 JUKEBOX / v4 DELAY
  kGarbage = 3001,  // XDR decode violation -> RPC GARBAGE_ARGS
  kEof = 3002,      // orderly connection shutdown (transport-internal)
  kBadHandle = 3003,  // authenticated file-handle envelope is malformed/forged
};

constexpr Errno errno_from(int e) { return static_cast<Errno>(e); }
// io primitives return negative errno on failure (design 02 §2.2)
constexpr Errno errno_from_neg(int r) { return static_cast<Errno>(-r); }
constexpr int raw(Errno e) { return static_cast<int>(e); }

inline std::string errno_name(Errno e) {
  switch (e) {
    case Errno::kOk: return "OK";
    case Errno::kJukebox: return "JUKEBOX";
    case Errno::kGarbage: return "GARBAGE";
    case Errno::kEof: return "EOF";
    case Errno::kBadHandle: return "BADHANDLE";
    default: return std::string(strerrorname_np(raw(e)) ? strerrorname_np(raw(e)) : "E?");
  }
}

}  // namespace lnfs
