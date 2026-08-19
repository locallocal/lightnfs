// Non-libFuzzer driver: replays corpus files (or a built-in smoke set) through the fuzz
// entry so gcc/sanitizer CI configurations still cover the path.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

extern "C" void lnfs_fuzz_entry(const uint8_t* data, size_t size);

int main(int argc, char** argv) {
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
      std::ifstream f(argv[i], std::ios::binary);
      std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
      lnfs_fuzz_entry(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
      std::printf("ok: %s (%zu bytes)\n", argv[i], bytes.size());
    }
    return 0;
  }
  // built-in smoke inputs: empty, garbage, truncated header, plausible call
  const uint8_t empty[] = {0};
  lnfs_fuzz_entry(empty, 0);
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
