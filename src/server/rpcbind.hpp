#pragma once

#include <cstdint>

#include "util/result.hpp"

namespace lnfs::server {

// Register one TCP RPC program/version with the local rpcbind/portmapper v2 service.
Result<void> rpcbind_set(uint32_t program, uint32_t version, uint16_t port);
Result<void> rpcbind_unset(uint32_t program, uint32_t version);

}  // namespace lnfs::server
