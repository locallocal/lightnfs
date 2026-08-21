#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include "core/config.hpp"

namespace lnfs::core {

struct DecodedHandle {
  ExportEntry* export_entry = nullptr;
  backend::ObjId oid;
};

class FileHandleCodec {
 public:
  static constexpr uint8_t kVersion = 1;
  static Result<FileHandleCodec> load_or_create(const std::string& state_dir);
  static FileHandleCodec from_key(std::array<std::byte, 16> key, ExportTable& exports) {
    return FileHandleCodec(key, exports);
  }

  std::vector<std::byte> encode(const ExportEntry& exp, const backend::ObjId& oid) const;
  Result<DecodedHandle> decode(std::span<const std::byte> fh,
                               const sockaddr_storage& peer) const;
  void bind(ExportTable& exports) { exports_ = &exports; }

  // Offline decode for lightnfs-fh (design 08 §8.6): no export table, no IP checks.
  struct Inspection {
    uint8_t version = 0;
    uint32_t fsid = 0;
    backend::ObjId oid;
    bool hmac_ok = false;
  };
  static FileHandleCodec from_key_only(std::array<std::byte, 16> key) {
    return FileHandleCodec(key);
  }
  Result<Inspection> inspect(std::span<const std::byte> fh) const;

 private:
  FileHandleCodec(std::array<std::byte, 16> key, ExportTable& exports)
      : key_(key), exports_(&exports) {}
  explicit FileHandleCodec(std::array<std::byte, 16> key) : key_(key) {}
  uint64_t tag(std::span<const std::byte> bytes) const;

  std::array<std::byte, 16> key_{};
  ExportTable* exports_ = nullptr;
};

}  // namespace lnfs::core
