// Fuzz target for core/file_handle decode (plan doc 10 §7.2): handle bytes are fully
// client-controlled and lightnfs-fh feeds operator hex straight into the codec, so
// decode/decode_v4/inspect must hold up against arbitrary input — including inputs one
// bit-flip away from a validly HMAC'd handle (exercised via an in-process re-encode).
//
// Hermetic: fixed key, in-memory export table, no filesystem, no runtime.

#include <arpa/inet.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

#include "backend/memory/memory.hpp"
#include "core/config.hpp"
#include "core/file_handle.hpp"
#include "util/log.hpp"

using namespace lnfs;

namespace {

struct Env {
  core::ExportTable exports;
  std::unique_ptr<core::FileHandleCodec> codec;
  sockaddr_storage peer{};
  std::vector<std::byte> valid;  // a correctly tagged handle to mutate

  Env() {
    lnfs::set_log_level(lnfs::LogLevel::kError);
    core::ExportConfig cfg;
    cfg.path = "/fuzz";
    cfg.fsid = 1;
    cfg.clients = {"127.0.0.0/8"};
    auto memory = std::make_unique<backend::MemoryBackend>(1);
    (void)exports.add(cfg, std::move(memory));
    std::array<std::byte, 16> key{};
    key[0] = std::byte{0x5a};
    codec = std::make_unique<core::FileHandleCodec>(
        core::FileHandleCodec::from_key(key, exports));
    auto* sin = reinterpret_cast<sockaddr_in*>(&peer);
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &sin->sin_addr);
    backend::ObjId oid{};
    oid.len = 8;
    for (int i = 0; i < 8; ++i) oid.bytes[i] = std::byte(i + 1);
    valid = codec->encode(*exports.by_fsid(1), oid);
  }
};

Env& env() {
  static Env e;
  return e;
}

}  // namespace

extern "C" void lnfs_fuzz_entry(const uint8_t* data, size_t size) {
  auto& e = env();
  std::span<const std::byte> fh(reinterpret_cast<const std::byte*>(data), size);
  (void)e.codec->decode(fh, e.peer);
  (void)e.codec->decode_v4(fh, e.peer);
  (void)e.codec->inspect(fh);

  // Near-valid input: apply the fuzzer's bytes as targeted mutations to a handle that
  // carries a correct tag, reaching the post-HMAC parse stages more often.
  if (size >= 2) {
    auto mutated = e.valid;
    mutated[data[0] % mutated.size()] ^= std::byte(data[1]);
    std::span<const std::byte> mfh(mutated.data(), mutated.size());
    (void)e.codec->decode(mfh, e.peer);
    (void)e.codec->decode_v4(mfh, e.peer);
    (void)e.codec->inspect(mfh);
  }
}

#ifndef LNFS_FUZZ_REGRESS
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lnfs_fuzz_entry(data, size);
  return 0;
}
#endif
