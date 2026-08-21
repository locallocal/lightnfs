#include "nfsv4/nfs4_types.hpp"

#include <algorithm>

namespace lnfs::nfsv4 {

bool Stateid::is_all_zero() const {
  if (seqid != 0) return false;
  return std::all_of(other.begin(), other.end(),
                     [](std::byte b) { return b == std::byte{0}; });
}

bool Stateid::is_all_one() const {
  if (seqid != 0xffffffffu) return false;
  return std::all_of(other.begin(), other.end(),
                     [](std::byte b) { return b == std::byte{0xff}; });
}

void Stateid::encode(xdr::XdrEnc& enc) const {
  enc.u32(seqid);
  enc.opaque_fixed(other);
}

Result<Stateid> Stateid::decode(xdr::XdrDec& dec) {
  Stateid out;
  out.seqid = LNFS_TRY(dec.u32());
  auto bytes = LNFS_TRY(dec.opaque_fixed(12));
  std::copy(bytes.begin(), bytes.end(), out.other.begin());
  return out;
}

void Bitmap::encode(xdr::XdrEnc& enc) const {
  enc.u32(static_cast<uint32_t>(words.size()));
  for (uint32_t w : words) enc.u32(w);
}

Result<Bitmap> Bitmap::decode(xdr::XdrDec& dec) {
  uint32_t count = LNFS_TRY(dec.u32());
  if (count > 8) return Err(Errno::kGarbage);  // attrs stop well below 8 words
  Bitmap out;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t word = LNFS_TRY(dec.u32());
    if (i < 3) out.words.push_back(word);
    else if (word != 0) return Err(Errno::kGarbage);  // bits we can never serve
  }
  return out;
}

void encode_nfstime(xdr::XdrEnc& enc, const backend::Timespec& time) {
  enc.u64(static_cast<uint64_t>(time.sec));  // int64 on the wire
  enc.u32(std::min(time.nsec, 999999999u));
}

void ChannelAttrs::encode(xdr::XdrEnc& enc) const {
  enc.u32(headerpad);
  enc.u32(max_request);
  enc.u32(max_response);
  enc.u32(max_response_cached);
  enc.u32(max_ops);
  enc.u32(max_requests);
  enc.u32(0);  // rdma_ird: empty array
}

Result<ChannelAttrs> ChannelAttrs::decode(xdr::XdrDec& dec) {
  ChannelAttrs out;
  out.headerpad = LNFS_TRY(dec.u32());
  out.max_request = LNFS_TRY(dec.u32());
  out.max_response = LNFS_TRY(dec.u32());
  out.max_response_cached = LNFS_TRY(dec.u32());
  out.max_ops = LNFS_TRY(dec.u32());
  out.max_requests = LNFS_TRY(dec.u32());
  uint32_t rdma = LNFS_TRY(dec.u32());
  if (rdma > 1) return Err(Errno::kGarbage);
  if (rdma == 1) (void)LNFS_TRY(dec.u32());
  return out;
}

const char* op_name(uint32_t op) {
  switch (static_cast<Op>(op)) {
    case Op::kAccess: return "ACCESS";
    case Op::kClose: return "CLOSE";
    case Op::kCommit: return "COMMIT";
    case Op::kCreate: return "CREATE";
    case Op::kGetattr: return "GETATTR";
    case Op::kGetfh: return "GETFH";
    case Op::kLink: return "LINK";
    case Op::kLock: return "LOCK";
    case Op::kLockt: return "LOCKT";
    case Op::kLocku: return "LOCKU";
    case Op::kLookup: return "LOOKUP";
    case Op::kLookupp: return "LOOKUPP";
    case Op::kNverify: return "NVERIFY";
    case Op::kOpen: return "OPEN";
    case Op::kOpenattr: return "OPENATTR";
    case Op::kOpenDowngrade: return "OPEN_DOWNGRADE";
    case Op::kPutfh: return "PUTFH";
    case Op::kPutpubfh: return "PUTPUBFH";
    case Op::kPutrootfh: return "PUTROOTFH";
    case Op::kRead: return "READ";
    case Op::kReaddir: return "READDIR";
    case Op::kReadlink: return "READLINK";
    case Op::kRemove: return "REMOVE";
    case Op::kRename: return "RENAME";
    case Op::kRestorefh: return "RESTOREFH";
    case Op::kSavefh: return "SAVEFH";
    case Op::kSecinfo: return "SECINFO";
    case Op::kSetattr: return "SETATTR";
    case Op::kVerify: return "VERIFY";
    case Op::kWrite: return "WRITE";
    case Op::kBindConnToSession: return "BIND_CONN_TO_SESSION";
    case Op::kExchangeId: return "EXCHANGE_ID";
    case Op::kCreateSession: return "CREATE_SESSION";
    case Op::kDestroySession: return "DESTROY_SESSION";
    case Op::kFreeStateid: return "FREE_STATEID";
    case Op::kSecinfoNoName: return "SECINFO_NO_NAME";
    case Op::kSequence: return "SEQUENCE";
    case Op::kTestStateid: return "TEST_STATEID";
    case Op::kDestroyClientid: return "DESTROY_CLIENTID";
    case Op::kReclaimComplete: return "RECLAIM_COMPLETE";
    default: return "?";
  }
}

}  // namespace lnfs::nfsv4
