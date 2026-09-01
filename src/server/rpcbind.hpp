#pragma once

#include <cstdint>

#include "util/result.hpp"

namespace lnfs::server {

// Register one TCP RPC program/version with the local rpcbind/portmapper v2 service.
Result<void> rpcbind_set(uint32_t program, uint32_t version, uint16_t port);
Result<void> rpcbind_unset(uint32_t program, uint32_t version);

// Test seam (plan doc 10 §7.1): where the portmapper listens on 127.0.0.1 (default 111).
// Unit tests point this at a fake responder on an ephemeral port.
void rpcbind_target_port(uint16_t port);

}  // namespace lnfs::server
