#include "core/names.hpp"

namespace lnfs::core {

NameCheck check_component(std::string_view name, size_t max_len) {
  if (name.empty()) return NameCheck::kEmpty;
  if (name.size() > max_len) return NameCheck::kTooLong;
  if (name.find('/') != std::string_view::npos || name.find('\0') != std::string_view::npos)
    return NameCheck::kBadChar;
  if (name == "." || name == "..") return NameCheck::kDot;
  return NameCheck::kOk;
}

bool valid_utf8(std::span<const std::byte> bytes) {
  size_t i = 0, n = bytes.size();
  while (i < n) {
    uint8_t c = static_cast<uint8_t>(bytes[i]);
    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3
                 : (c >> 3) == 0x1E ? 4 : 0;
    if (len == 0 || i + len > n) return false;
    for (size_t k = 1; k < len; ++k)
      if ((static_cast<uint8_t>(bytes[i + k]) & 0xC0) != 0x80) return false;
    if (len == 2 && c < 0xC2) return false;  // overlong
    if (len == 3 && c == 0xE0 && static_cast<uint8_t>(bytes[i + 1]) < 0xA0) return false;
    if (len == 3 && c == 0xED && static_cast<uint8_t>(bytes[i + 1]) > 0x9F)
      return false;  // UTF-16 surrogates
    if (len == 3 && c == 0xEF && static_cast<uint8_t>(bytes[i + 1]) == 0xBF &&
        (static_cast<uint8_t>(bytes[i + 2]) == 0xBE ||
         static_cast<uint8_t>(bytes[i + 2]) == 0xBF))
      return false;  // U+FFFE / U+FFFF
    if (len == 4 && c == 0xF0 && static_cast<uint8_t>(bytes[i + 1]) < 0x90) return false;
    if (len == 4 && (c > 0xF4 || (c == 0xF4 && static_cast<uint8_t>(bytes[i + 1]) > 0x8F)))
      return false;
    i += len;
  }
  return true;
}

}  // namespace lnfs::core
