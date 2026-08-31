#pragma once
// Name component discipline shared by v3, v4 and mountd (plan doc 10 §6.2).  One
// judgement of empty / too long / '/' or NUL / "." ".." — each protocol maps the verdict
// onto its own status code (v3 ACCES, v4 INVAL/BADNAME, mountd INVAL).  UTF-8 validation
// is a v4-only requirement (RFC 8881 §14.1) but lives here so it has one implementation.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace lnfs::core {

inline constexpr size_t kMaxNameLen = 255;

enum class NameCheck : uint8_t {
  kOk,
  kEmpty,    // zero-length component
  kTooLong,  // longer than max_len
  kBadChar,  // contains '/' or NUL
  kDot,      // "." or ".." — legal to look up, never to create/remove
};

// Evaluation order: empty -> length -> characters -> dots.
NameCheck check_component(std::string_view name, size_t max_len = kMaxNameLen);

// kOk, or kDot as well when the caller may name "." / ".." (v3 LOOKUP).
inline bool valid_component(std::string_view name, bool allow_dots = false,
                            size_t max_len = kMaxNameLen) {
  NameCheck c = check_component(name, max_len);
  return c == NameCheck::kOk || (allow_dots && c == NameCheck::kDot);
}

// Strict UTF-8: rejects overlong forms, UTF-16 surrogates, U+FFFE/U+FFFF and > U+10FFFF.
bool valid_utf8(std::span<const std::byte> bytes);
inline bool valid_utf8(std::string_view s) {
  return valid_utf8(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(s.data()), s.size()));
}

}  // namespace lnfs::core
