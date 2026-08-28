#pragma once
// CB_COMPOUND construction and reply decoding (RFC 8881 §20, plan doc 10 §5.2).
// The server-side callbacks lightnfs sends: CB_SEQUENCE-prefixed CB_RECALL (read
// delegation recall) and CB_NOTIFY_LOCK (a contended lock may be free).  Records are
// built as plain byte vectors — the state layer has no buffer pool, and callbacks are
// rare enough that a heap vector per call is the right trade.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "nfsv4/nfs4_types.hpp"

namespace lnfs::nfsv4::cb {

// Callback credential: the first AUTH_SYS the client offered in CREATE_SESSION
// csa_sec_parms, else AUTH_NONE (RFC 8881 §2.10.8.2).
struct Cred {
  bool auth_sys = false;
  uint32_t uid = 0, gid = 0;
  std::string machine;
};

struct Target {
  uint32_t xid = 0;
  uint32_t program = 0;  // client's cb_program from CREATE_SESSION
  Cred cred;
  SessionId sessionid{};
  uint32_t slot_seq = 0;  // CB_SEQUENCE csa_sequenceid (slot 0; we advertise 1 slot)
};

// Full RPC CALL records (no record marking).
std::vector<std::byte> build_cb_recall(const Target& t, const Stateid& sid,
                                       std::span<const std::byte> fh);
std::vector<std::byte> build_cb_notify_lock(const Target& t, std::span<const std::byte> fh,
                                            uint64_t clientid,
                                            std::span<const std::byte> owner);

// Decodes an RPC reply record for a CB_COMPOUND: accepted+success with the compound
// status extracted.  A parse failure returns nfsv4 BADXDR; RPC-level rejection maps
// to a nonzero status too — callers only distinguish zero from nonzero.
struct ReplyStatus {
  bool rpc_ok = false;       // RPC accepted with SUCCESS
  uint32_t nfs_status = ~0u; // CB_COMPOUND status when rpc_ok
};
ReplyStatus parse_cb_reply(std::span<const std::byte> record);

}  // namespace lnfs::nfsv4::cb
