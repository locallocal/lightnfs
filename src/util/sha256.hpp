#pragma once
// SHA-256 (FIPS 180-4): a self-contained implementation for the cluster export digest
// (design 09 §9.3, plan 10 B4).  Not for anything hot: the whole input is hashed in
// one call.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace lnfs::util {

using Sha256Digest = std::array<std::uint8_t, 32>;

Sha256Digest sha256(std::span<const std::byte> data);
inline Sha256Digest sha256(std::string_view text) {
  return sha256(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                           text.size()));
}
// Lowercase hex, 64 characters.
std::string sha256_hex(std::span<const std::byte> data);
inline std::string sha256_hex(std::string_view text) {
  return sha256_hex(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

}  // namespace lnfs::util
