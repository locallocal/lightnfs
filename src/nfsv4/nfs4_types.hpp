#pragma once
// NFSv4.1 wire types — the skeleton subset for the read-only milestone (design 04 §4.5,
// nfsv4 research 02/03/06/10).  Only what the implemented operations need; everything
// else stays out until its phase.

#include <array>
#include <cstdint>
#include <string>

#include "backend/api.hpp"
#include "util/small_vec.hpp"
#include "xdr/xdr.hpp"

namespace lnfs::nfsv4 {

inline constexpr uint32_t kProgram = 100003;
inline constexpr uint32_t kVersion = 4;
inline constexpr uint32_t kMaxFileHandle = 128;  // NFS4_FHSIZE
inline constexpr uint32_t kMaxName = 255;
inline constexpr uint32_t kMaxTag = 1024;
inline constexpr uint32_t kMaxOwnerId = 1024;    // EXCHANGE_ID co_ownerid
inline constexpr uint32_t kLeaseSeconds = 90;

// ---- operation codes (nfsv4 research 02 §2.4) ----
enum class Op : uint32_t {
  kAccess = 3,
  kClose = 4,
  kCommit = 5,
  kCreate = 6,
  kGetattr = 9,
  kGetfh = 10,
  kLink = 11,
  kLock = 12,
  kLockt = 13,
  kLocku = 14,
  kLookup = 15,
  kLookupp = 16,
  kNverify = 17,
  kOpen = 18,
  kOpenattr = 19,
  kOpenDowngrade = 21,
  kPutfh = 22,
  kPutpubfh = 23,
  kPutrootfh = 24,
  kRead = 25,
  kReaddir = 26,
  kReadlink = 27,
  kRemove = 28,
  kRename = 29,
  kRestorefh = 31,
  kSavefh = 32,
  kSecinfo = 33,
  kSetattr = 34,
  kVerify = 37,
  kWrite = 38,
  kBindConnToSession = 41,
  kExchangeId = 42,
  kCreateSession = 43,
  kDestroySession = 44,
  kFreeStateid = 45,
  kSecinfoNoName = 52,
  kSequence = 53,
  kTestStateid = 55,
  kDestroyClientid = 57,
  kReclaimComplete = 58,
  kIllegal = 10044,
};
inline constexpr uint32_t kFirstOp = 3;
inline constexpr uint32_t kLastKnownOp = 71;  // 4.2 ceiling; beyond -> ILLEGAL

// ---- status codes (nfsv4 research 10) ----
enum class Status : uint32_t {
  kOk = 0,
  kPerm = 1,
  kNoent = 2,
  kIo = 5,
  kNxio = 6,
  kAccess = 13,
  kExist = 17,
  kXdev = 18,
  kNotdir = 20,
  kIsdir = 21,
  kInval = 22,
  kFbig = 27,
  kNospc = 28,
  kRofs = 30,
  kMlink = 31,
  kNametoolong = 63,
  kNotempty = 66,
  kDquot = 69,
  kStale = 70,
  kBadhandle = 10001,
  kBadCookie = 10003,
  kNotsupp = 10004,
  kToosmall = 10005,
  kServerfault = 10006,
  kBadtype = 10007,
  kDelay = 10008,
  kSame = 10009,
  kDenied = 10010,
  kExpired = 10011,
  kGrace = 10013,
  kFhexpired = 10014,
  kShareDenied = 10015,
  kWrongsec = 10016,
  kClidInuse = 10017,
  kResource = 10018,
  kNofilehandle = 10020,
  kMinorVersMismatch = 10021,
  kStaleClientid = 10022,
  kStaleStateid = 10023,
  kOldStateid = 10024,
  kBadStateid = 10025,
  kNotSame = 10027,
  kSymlink = 10029,
  kRestorefh = 10030,
  kNoGrace = 10033,
  kReclaimBad = 10034,
  kBadxdr = 10036,
  kOpenmode = 10038,
  kBadname = 10041,
  kOpIllegal = 10044,
  kBadsession = 10052,
  kBadslot = 10053,
  kCompleteAlready = 10054,
  kConnNotBound = 10055,
  kSeqMisordered = 10063,
  kSequencePos = 10064,
  kReqTooBig = 10065,
  kRepTooBig = 10066,
  kRepTooBigToCache = 10067,
  kRetryUncachedRep = 10068,
  kTooManyOps = 10070,
  kNotOnlyOp = 10081,
  kOpNotInSession = 10071,
  kClientidBusy = 10074,
  kBadHighSlot = 10077,
  kWrongType = 10083,
};

// ---- core wire structures ----

struct Stateid {
  uint32_t seqid = 0;
  std::array<std::byte, 12> other{};

  bool is_all_zero() const;
  bool is_all_one() const;  // READ bypass stateid
  bool is_special() const { return is_all_zero() || is_all_one(); }
  void encode(xdr::XdrEnc& enc) const;
  static Result<Stateid> decode(xdr::XdrDec& dec);
  friend bool operator==(const Stateid&, const Stateid&) = default;
};

using SessionId = std::array<std::byte, 16>;
using Verifier = std::array<std::byte, 8>;

// bitmap4: variable-length word array; we never need more than 3 words (attrs < 96).
struct Bitmap {
  SmallVec<uint32_t, 3> words;

  bool test(uint32_t bit) const {
    uint32_t w = bit / 32;
    return w < words.size() && (words[w] >> (bit % 32)) & 1;
  }
  void set(uint32_t bit) {
    uint32_t w = bit / 32;
    while (words.size() <= w) words.push_back(0);
    words[w] |= 1u << (bit % 32);
  }
  bool empty() const {
    for (uint32_t w : words)
      if (w) return false;
    return true;
  }
  uint32_t highest_word() const { return static_cast<uint32_t>(words.size()); }

  void encode(xdr::XdrEnc& enc) const;
  static Result<Bitmap> decode(xdr::XdrDec& dec);
};

void encode_nfstime(xdr::XdrEnc& enc, const backend::Timespec& time);

// channel attrs (CREATE_SESSION)
struct ChannelAttrs {
  uint32_t headerpad = 0;
  uint32_t max_request = 1u << 20;
  uint32_t max_response = 1u << 20;
  uint32_t max_response_cached = 8u << 10;
  uint32_t max_ops = 16;
  uint32_t max_requests = 32;  // slot count
  void encode(xdr::XdrEnc& enc) const;
  static Result<ChannelAttrs> decode(xdr::XdrDec& dec);
};

const char* op_name(uint32_t op);

}  // namespace lnfs::nfsv4
