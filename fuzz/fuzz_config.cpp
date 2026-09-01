// Fuzz target for the hand-rolled TOML subset parser (plan doc 10 §7.2):
// core::parse_config consumes operator-supplied text; any crash, UB or unbounded
// allocation on malformed input is a finding.  Hermetic: string in, Result out —
// validate_config/ExportTable::build are deliberately not called (they stat() paths).

#include <cstdint>
#include <string_view>

#include "core/config.hpp"

extern "C" void lnfs_fuzz_entry(const uint8_t* data, size_t size) {
  std::string_view text(reinterpret_cast<const char*>(data), size);
  (void)lnfs::core::parse_config(text);
}

#ifndef LNFS_FUZZ_REGRESS
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lnfs_fuzz_entry(data, size);
  return 0;
}
#endif
