// Fuzz target for the v4 attrs bitmap + settable-fattr decoder (plan doc 10 §7.2):
// SETATTR/OPEN(create)/CREATE feed client bytes into Bitmap::decode and
// decode_settable_fattr (nested XdrDec over the attr_vals opaque).  Hermetic: pure
// decode, no engine, no filesystem.

#include <cstdint>
#include <span>

#include "nfsv4/attrs.hpp"
#include "nfsv4/nfs4_types.hpp"
#include "xdr/xdr.hpp"

using namespace lnfs;

extern "C" void lnfs_fuzz_entry(const uint8_t* data, size_t size) {
  std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
  {
    xdr::XdrDec dec(bytes);
    backend::SetAttr out;
    nfsv4::Bitmap set;
    (void)nfsv4::decode_settable_fattr(dec, out, set);
  }
  {
    // Bitmap decode/encode round trip on its own: the word-count path.
    xdr::XdrDec dec(bytes);
    (void)nfsv4::Bitmap::decode(dec);
  }
}

#ifndef LNFS_FUZZ_REGRESS
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lnfs_fuzz_entry(data, size);
  return 0;
}
#endif
