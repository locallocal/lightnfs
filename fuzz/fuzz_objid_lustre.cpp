// Fuzz target for the Lustre backend handle codec (design 06 §6.5 / plan doc 10 §7.2):
// ObjId bytes come straight out of client filehandles; LustreBackend::fid_from_oid is
// the parse boundary before the FID is rendered into a .lustre/fid path.  Raw input
// goes through ObjId::from + fid_from_oid; anything that parses must round-trip
// through oid_from_fid to the identical ObjId (P1: the codec is a bijection on valid
// input) and render to a bounded, bracket-free FID string.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

#include "backend/lustre/lustre.hpp"

using namespace lnfs;

extern "C" void lnfs_fuzz_entry(const uint8_t* data, size_t size) {
  auto oid = backend::ObjId::from(
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size));
  if (!oid) return;
  auto fid = backend::LustreBackend::fid_from_oid(*oid);
  if (!fid) return;
  auto back = backend::LustreBackend::oid_from_fid(*fid);
  if (!(back == *oid)) std::abort();
  auto text = backend::llapi::fid_to_string(*fid);
  if (text.empty() || text.size() > 40 || text.find('[') != std::string::npos) std::abort();
}

#ifndef LNFS_FUZZ_REGRESS
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lnfs_fuzz_entry(data, size);
  return 0;
}
#endif
