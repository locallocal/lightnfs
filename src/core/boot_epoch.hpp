#pragma once
// Boot epoch persistence (design 04 §4.2, roadmap 4.2): a counter in state_dir bumped on
// every start.  Its value becomes the v3 write/commit verifier, so clients detect a
// server restart and resend uncommitted UNSTABLE writes.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "util/result.hpp"

namespace lnfs::core {

// Reads state_dir/boot_epoch, increments it, persists durably (write + fsync + rename),
// and returns the new value.  First start yields 1.
Result<uint64_t> bump_boot_epoch(const std::string& state_dir);

using WriteVerf = std::array<std::byte, 8>;
WriteVerf verifier_from_epoch(uint64_t epoch);
// Active-active (design 10 §10.5, plan 12 E1): every gateway keeps its own epoch
// (epoch.<node>), so two gateways can share an epoch value — and a client whose export
// migrated between them must still see the verifier change and resend UNSTABLE data.
// The node name goes into the high half; the epoch stays in the low half (a restart
// still flips it).
WriteVerf verifier_for_node(uint64_t epoch, std::string_view node);

}  // namespace lnfs::core
