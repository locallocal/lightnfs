// Non-libFuzzer driver: replays seed/corpus files (or a built-in smoke set) through the
// fuzz entry so gcc/sanitizer CI configurations still cover the path.  Linked once per
// fuzz target (CMake builds fuzz_regress_<target> from this file + the target file).
//
// Usage: fuzz_regress_<t> [file|dir]...   — a directory replays every regular file in
// it and is silently skipped when absent (seed dirs are optional); with no arguments a
// generic built-in smoke set runs (empty, garbage, truncations, an RPC-shaped call).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

extern "C" void lnfs_fuzz_entry(const uint8_t* data, size_t size);

namespace {

int replay_file(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "cannot read %s\n", path.c_str());
    return 1;
  }
  std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
  lnfs_fuzz_entry(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  std::printf("ok: %s (%zu bytes)\n", path.c_str(), bytes.size());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    int rc = 0;
    size_t replayed = 0;
    for (int i = 1; i < argc; ++i) {
      std::error_code ec;
      if (std::filesystem::is_directory(argv[i], ec)) {
        for (const auto& ent : std::filesystem::directory_iterator(argv[i])) {
          if (!ent.is_regular_file()) continue;
          rc |= replay_file(ent.path());
          ++replayed;
        }
      } else if (std::filesystem::exists(argv[i], ec)) {
        rc |= replay_file(argv[i]);
        ++replayed;
      }  // absent seed dirs are fine — not every target has one yet
    }
    std::printf("fuzz_regress: %zu input(s) replayed\n", replayed);
    return rc;
  }
  // Built-in smoke inputs: empty, garbage, an RPC-shaped call plus all its truncations.
  // Generic on purpose — every entry must survive arbitrary bytes.
  lnfs_fuzz_entry(nullptr, 0);
  const uint8_t garbage[] = {0xde, 0xad, 0xbe, 0xef, 0x01};
  lnfs_fuzz_entry(garbage, sizeof(garbage));
  uint8_t call[64] = {};
  auto put32 = [&](int off, uint32_t v) {
    call[off] = v >> 24;
    call[off + 1] = v >> 16;
    call[off + 2] = v >> 8;
    call[off + 3] = v;
  };
  put32(0, 0x1234);   // xid
  put32(4, 0);        // CALL
  put32(8, 2);        // rpcvers
  put32(12, 300000);  // prog
  put32(16, 1);       // vers
  put32(20, 0);       // proc
  // cred/verf AUTH_NONE with zero-length bodies
  put32(24, 0);
  put32(28, 0);
  put32(32, 0);
  put32(36, 0);
  lnfs_fuzz_entry(call, 40);
  for (size_t trunc = 0; trunc < 40; ++trunc) lnfs_fuzz_entry(call, trunc);
  std::printf("fuzz_regress: built-in smoke inputs ok\n");
  return 0;
}
