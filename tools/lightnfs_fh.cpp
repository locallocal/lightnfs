// lightnfs-fh (design 08 §8.6): decodes a hex-encoded lightnfs file handle for
// wireshark-assisted debugging.
//
//   lightnfs-fh [--key STATE_DIR/hmac.key] <hex-handle>
//
// Prints version, fsid, backend ObjId (hex) and, when the HMAC key file is supplied,
// whether the SipHash authentication tag verifies.

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/file_handle.hpp"

namespace {

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string key_path;
  int argi = 1;
  if (argi + 1 < argc && std::string(argv[argi]) == "--key") {
    key_path = argv[argi + 1];
    argi += 2;
  }
  if (argi >= argc) {
    std::fprintf(stderr, "usage: lightnfs-fh [--key HMAC_KEY_FILE] <hex-handle>\n");
    return 2;
  }
  std::string hex = argv[argi];
  std::erase_if(hex, [](char c) { return c == ':' || c == ' '; });
  if (hex.size() % 2 != 0) {
    std::fprintf(stderr, "odd hex length\n");
    return 2;
  }
  std::vector<std::byte> fh;
  for (size_t i = 0; i < hex.size(); i += 2) {
    int hi = hex_nibble(hex[i]), lo = hex_nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      std::fprintf(stderr, "invalid hex at offset %zu\n", i);
      return 2;
    }
    fh.push_back(static_cast<std::byte>(hi << 4 | lo));
  }

  std::array<std::byte, 16> key{};
  bool have_key = false;
  if (!key_path.empty()) {
    int fd = ::open(key_path.c_str(), O_RDONLY);
    if (fd < 0 || ::read(fd, key.data(), key.size()) != static_cast<ssize_t>(key.size())) {
      std::fprintf(stderr, "cannot read 16-byte key from %s\n", key_path.c_str());
      if (fd >= 0) ::close(fd);
      return 1;
    }
    ::close(fd);
    have_key = true;
  }

  auto codec = lnfs::core::FileHandleCodec::from_key_only(key);
  auto info = codec.inspect(fh);
  if (!info) {
    std::fprintf(stderr, "not a lightnfs handle (length %zu)\n", fh.size());
    return 1;
  }
  std::printf("length:  %zu bytes\n", fh.size());
  std::printf("version: %u%s\n", info->version,
              info->version == lnfs::core::FileHandleCodec::kVersion ? "" : "  (UNKNOWN)");
  std::printf("fsid:    %u\n", info->fsid);
  std::printf("objid:   ");
  for (std::byte b : info->oid.view()) std::printf("%02x", static_cast<unsigned>(b));
  std::printf(" (%u bytes)\n", info->oid.len);
  if (have_key)
    std::printf("hmac:    %s\n", info->hmac_ok ? "VALID" : "INVALID");
  else
    std::printf("hmac:    (no key supplied; pass --key STATE_DIR/hmac.key to verify)\n");
  return 0;
}
