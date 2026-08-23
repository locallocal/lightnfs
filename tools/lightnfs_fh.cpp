// lightnfs-fh (design 08 §8.6): decodes a hex-encoded lightnfs file handle for
// wireshark-assisted debugging.
//
//   lightnfs-fh <hex-handle> [--key=STATE_DIR/hmac.key]
//
// Prints version, fsid, backend ObjId (hex) and, when the HMAC key file is supplied,
// whether the SipHash authentication tag verifies. cflag takes long-option values
// only as --name=value; the historical `--key PATH` spelling (including before the
// handle) is folded by normalize_argv.

#include <fcntl.h>
#include <unistd.h>

#include <ccmd.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/file_handle.hpp"

namespace {

using Cmd = std::shared_ptr<ccmd::c_command>;

// ccmd callbacks return void; the exit code travels through this
// (0 success / 1 runtime failure / 2 usage error).
int g_exit = 0;

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int inspect_handle(const std::string& key_path, std::string hex) {
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
    std::printf("hmac:    (no key supplied; pass --key=STATE_DIR/hmac.key to verify)\n");
  return 0;
}

// Folds `--key PATH`/`-k PATH` into --key=PATH (the only form cflag takes).
std::vector<std::string> normalize_argv(int argc, char** argv) {
  std::vector<std::string> out;
  out.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    std::string a = argv[i];
    if ((a == "--key" || a == "-k") && i + 1 < argc) {
      out.push_back("--key=" + std::string(argv[++i]));
    } else {
      out.push_back(std::move(a));
    }
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  auto root = std::make_shared<ccmd::c_command>(
      "lightnfs-fh", "lightnfs-fh 01000000010009... --key=/var/lib/lightnfs/hmac.key",
      "lightnfs-fh <hex-handle> [--key=HMAC_KEY_FILE]",
      "Offline decoder for lightnfs file handles (wireshark-assisted debugging): "
      "prints version, fsid and backend ObjId; with the server's hmac.key it also "
      "verifies the SipHash authentication tag. ':' and spaces in the hex are "
      "ignored, so wireshark copy-paste works as-is.",
      "file-handle decoder", [](const Cmd& c) {
        const auto& pos = c->args();
        if (pos.size() != 1) {
          c->print_help();
          g_exit = 2;
          return;
        }
        g_exit = inspect_handle(c->var<std::string>("key"), pos[0]);
      });
  root->varp<std::string>("key", "k", "",
                          "HMAC key file (STATE_DIR/hmac.key) to verify the tag");

  try {
    root->execute(normalize_argv(argc, argv));
    return g_exit;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lightnfs-fh: %s\n", e.what());
    return 2;
  }
}
