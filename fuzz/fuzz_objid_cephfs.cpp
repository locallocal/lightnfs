// Fuzz target for the CephFS backend handle codec (plan doc 10 §5.3 / §7.2): ObjId
// bytes come straight out of client filehandles; CephBackend::vino_from_oid is the
// parse boundary before the bytes reach ceph_ll_lookup_vino.  Raw input goes through
// ObjId::from + vino_from_oid; anything that parses must round-trip through
// oid_from_vino to the identical ObjId (P1: the codec is a bijection on valid input).

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

#include "backend/cephfs.hpp"

using namespace lnfs;

extern "C" void lnfs_fuzz_entry(const uint8_t* data, size_t size) {
  auto oid = backend::ObjId::from(
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size));
  if (!oid) return;
  auto vino = backend::CephBackend::vino_from_oid(*oid);
  if (!vino) return;
  if (vino->ino == 0) std::abort();  // inode 0 never parses
  auto back = backend::CephBackend::oid_from_vino(*vino);
  if (!(back == *oid)) std::abort();
}

#ifndef LNFS_FUZZ_REGRESS
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lnfs_fuzz_entry(data, size);
  return 0;
}
#endif
