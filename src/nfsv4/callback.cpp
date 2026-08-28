#include "nfsv4/callback.hpp"

#include <cstring>

namespace lnfs::nfsv4::cb {
namespace {

// CB op numbers (RFC 8881 §20).
constexpr uint32_t kCbRecall = 4;
constexpr uint32_t kCbSequence = 11;
constexpr uint32_t kCbNotifyLock = 14;

class Writer {
 public:
  void u32(uint32_t v) {
    for (int i = 24; i >= 0; i -= 8) out_.push_back(std::byte((v >> i) & 0xFF));
  }
  void u64(uint64_t v) {
    u32(static_cast<uint32_t>(v >> 32));
    u32(static_cast<uint32_t>(v));
  }
  void opaque(std::span<const std::byte> b) {
    u32(static_cast<uint32_t>(b.size()));
    raw(b);
    for (size_t pad = (4 - b.size() % 4) % 4; pad; --pad) out_.push_back(std::byte{0});
  }
  void raw(std::span<const std::byte> b) { out_.insert(out_.end(), b.begin(), b.end()); }
  std::vector<std::byte> take() { return std::move(out_); }

 private:
  std::vector<std::byte> out_;
};

std::span<const std::byte> bytes_of(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// RPC CALL header for prog=cb_program vers=1 proc=1 (CB_COMPOUND) with the callback
// credential the client asked for and a NULL verifier.
void rpc_call_header(Writer& w, const Target& t) {
  w.u32(t.xid);
  w.u32(0);  // CALL
  w.u32(2);  // rpcvers
  w.u32(t.program);
  w.u32(1);  // CB version (4.1 backchannel program version)
  w.u32(1);  // CB_COMPOUND
  if (t.cred.auth_sys) {
    Writer body;
    body.u32(0);  // stamp
    body.opaque(bytes_of(t.cred.machine));
    body.u32(t.cred.uid);
    body.u32(t.cred.gid);
    body.u32(0);  // no aux gids
    auto b = body.take();
    w.u32(1);  // AUTH_SYS
    w.opaque(b);
  } else {
    w.u32(0);  // AUTH_NONE
    w.u32(0);
  }
  w.u32(0);  // verf AUTH_NONE
  w.u32(0);
}

// CB_COMPOUND prefix: tag "", minorversion 1, callback_ident 0 (unused in 4.1),
// then CB_SEQUENCE on slot 0 with no referring call lists.
void compound_prefix(Writer& w, const Target& t, uint32_t numops) {
  w.u32(0);  // empty tag
  w.u32(1);  // minorversion
  w.u32(0);  // callback_ident
  w.u32(numops);
  w.u32(kCbSequence);
  w.raw(std::span<const std::byte>(t.sessionid.data(), t.sessionid.size()));
  w.u32(t.slot_seq);
  w.u32(0);  // slotid
  w.u32(0);  // highest_slotid
  w.u32(0);  // cachethis = false
  w.u32(0);  // no referring call lists
}

}  // namespace

std::vector<std::byte> build_cb_recall(const Target& t, const Stateid& sid,
                                       std::span<const std::byte> fh) {
  Writer w;
  rpc_call_header(w, t);
  compound_prefix(w, t, 2);
  w.u32(kCbRecall);
  w.u32(sid.seqid);
  w.raw(std::span<const std::byte>(sid.other.data(), sid.other.size()));
  w.u32(0);  // truncate = false
  w.opaque(fh);
  return w.take();
}

std::vector<std::byte> build_cb_notify_lock(const Target& t, std::span<const std::byte> fh,
                                            uint64_t clientid,
                                            std::span<const std::byte> owner) {
  Writer w;
  rpc_call_header(w, t);
  compound_prefix(w, t, 2);
  w.u32(kCbNotifyLock);
  w.opaque(fh);
  w.u64(clientid);
  w.opaque(owner);
  return w.take();
}

ReplyStatus parse_cb_reply(std::span<const std::byte> record) {
  ReplyStatus out;
  size_t off = 0;
  auto u32 = [&](uint32_t& v) {
    if (off + 4 > record.size()) return false;
    uint32_t be;
    std::memcpy(&be, record.data() + off, 4);
    off += 4;
    v = xdr::to_be32(be);  // symmetric swap
    return true;
  };
  uint32_t xid, mtype, reply_stat;
  if (!u32(xid) || !u32(mtype) || !u32(reply_stat)) return out;
  if (mtype != 1 || reply_stat != 0) return out;  // denied / not a reply
  uint32_t verf_flavor, verf_len;
  if (!u32(verf_flavor) || !u32(verf_len)) return out;
  off += (verf_len + 3) & ~3u;
  uint32_t accept_stat;
  if (!u32(accept_stat) || accept_stat != 0) return out;
  uint32_t status;
  if (!u32(status)) return out;  // CB_COMPOUND status
  out.rpc_ok = true;
  out.nfs_status = status;
  return out;
}

}  // namespace lnfs::nfsv4::cb
