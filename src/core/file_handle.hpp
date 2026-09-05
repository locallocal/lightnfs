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

// The 16-byte handle HMAC key at `path`: read when present, otherwise generated
// (getrandom) and created O_EXCL with mode 0600 — a concurrent creator loses the race
// and reads the winner's key.  Shared by the local state_dir and the cluster store
// (design 09 §9.3, plan 10 A2/A3).
Result<std::array<std::byte, 16>> load_or_create_hmac_key(const std::string& path);

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

  // v4 namespace decode (design 04 §4.3): fsid 0 is the pseudo-fs — browsable from any
  // source, no export/IP check (that happens when crossing into an export); fsid != 0
  // resolves the export and enforces the client CIDR like v3.
  struct DecodedV4 {
    uint32_t fsid = 0;
    backend::ObjId oid;
    ExportEntry* exp = nullptr;  // null for pseudo handles
  };
  Result<DecodedV4> decode_v4(std::span<const std::byte> fh,
                              const sockaddr_storage& peer) const;
  // Encode with an explicit fsid (0 = pseudo).
  std::vector<std::byte> encode_raw(uint32_t fsid, const backend::ObjId& oid) const;

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
