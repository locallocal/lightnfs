#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "backend/api.hpp"
#include "xdr/xdr.hpp"

namespace lnfs::nfsv3 {

inline constexpr uint32_t kProgram = 100003;
inline constexpr uint32_t kVersion = 3;
inline constexpr uint32_t kMaxFileHandle = 64;
inline constexpr uint32_t kMaxName = 255;
inline constexpr uint32_t kMaxPath = 1024;

enum class Proc : uint32_t {
  kNull = 0,
  kGetattr = 1,
  kSetattr = 2,
  kLookup = 3,
  kAccess = 4,
  kReadlink = 5,
  kRead = 6,
  kWrite = 7,
  kCreate = 8,
  kMkdir = 9,
  kSymlink = 10,
  kMknod = 11,
  kRemove = 12,
  kRmdir = 13,
  kRename = 14,
  kLink = 15,
  kReaddir = 16,
  kReaddirplus = 17,
  kFsstat = 18,
  kFsinfo = 19,
  kPathconf = 20,
  kCommit = 21,
};

enum class Status : uint32_t {
  kOk = 0,
  kPerm = 1,
  kNoent = 2,
  kIo = 5,
  kNxio = 6,
  kAcces = 13,
  kExist = 17,
  kXdev = 18,
  kNodev = 19,
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
  kRemote = 71,
  kBadhandle = 10001,
  kNotSync = 10002,
  kBadCookie = 10003,
  kNotsupp = 10004,
  kToosmall = 10005,
  kServerfault = 10006,
  kBadtype = 10007,
  kJukebox = 10008,
};

inline constexpr uint32_t kAccessRead = 0x0001;
inline constexpr uint32_t kAccessLookup = 0x0002;
inline constexpr uint32_t kAccessModify = 0x0004;
inline constexpr uint32_t kAccessExtend = 0x0008;
inline constexpr uint32_t kAccessDelete = 0x0010;
inline constexpr uint32_t kAccessExecute = 0x0020;
inline constexpr uint32_t kAccessAll = 0x003f;

inline constexpr uint32_t kFsfLink = 0x0001;
inline constexpr uint32_t kFsfSymlink = 0x0002;
inline constexpr uint32_t kFsfHomogeneous = 0x0008;
inline constexpr uint32_t kFsfCanSetTime = 0x0010;

struct FileHandle {
  std::vector<std::byte> data;
  void encode(xdr::XdrEnc& enc) const { enc.opaque(data); }
  static Result<FileHandle> decode(xdr::XdrDec& dec);
};

struct Diropargs {
  FileHandle dir;
  std::string name;
  void encode(xdr::XdrEnc& enc) const;
  static Result<Diropargs> decode(xdr::XdrDec& dec);
};

struct AccessArgs {
  FileHandle object;
  uint32_t access = 0;
  void encode(xdr::XdrEnc& enc) const;
  static Result<AccessArgs> decode(xdr::XdrDec& dec);
};

struct ReadArgs {
  FileHandle file;
  uint64_t offset = 0;
  uint32_t count = 0;
  void encode(xdr::XdrEnc& enc) const;
  static Result<ReadArgs> decode(xdr::XdrDec& dec);
};

struct ReaddirArgs {
  FileHandle dir;
  uint64_t cookie = 0;
  std::array<std::byte, 8> cookieverf{};
  uint32_t count = 0;
  void encode(xdr::XdrEnc& enc) const;
  static Result<ReaddirArgs> decode(xdr::XdrDec& dec);
};

struct ReaddirPlusArgs {
  FileHandle dir;
  uint64_t cookie = 0;
  std::array<std::byte, 8> cookieverf{};
  uint32_t dircount = 0;
  uint32_t maxcount = 0;
  void encode(xdr::XdrEnc& enc) const;
  static Result<ReaddirPlusArgs> decode(xdr::XdrDec& dec);
};

// ---- write-side arguments (RFC 1813) --------------------------------------

// sattr3 <-> backend::SetAttr (time_how: 0 DONT_CHANGE, 1 SERVER, 2 CLIENT).
Result<backend::SetAttr> decode_sattr(xdr::XdrDec& dec);
void encode_sattr(xdr::XdrEnc& enc, const backend::SetAttr& attrs);

struct SetattrArgs {
  FileHandle object;
  backend::SetAttr attrs;
  bool guard = false;
  backend::Timespec guard_ctime{};
  void encode(xdr::XdrEnc& enc) const;
  static Result<SetattrArgs> decode(xdr::XdrDec& dec);
};

inline constexpr uint32_t kUnstable = 0;
inline constexpr uint32_t kDataSync = 1;
inline constexpr uint32_t kFileSync = 2;

struct WriteArgs {
  FileHandle file;
  uint64_t offset = 0;
  uint32_t count = 0;
  uint32_t stable = kFileSync;
  // Zero-copy views into the decoded record, one per contiguous piece (plan doc 10
  // §2.4: a payload spanning recv buffers is handed to the backend as segments, never
  // flattened). data_len is their total size.
  SmallVec<std::span<const std::byte>, 8> data;
  uint32_t data_len = 0;
  static Result<WriteArgs> decode(xdr::XdrDec& dec);
};

inline constexpr uint32_t kCreateUnchecked = 0;
inline constexpr uint32_t kCreateGuarded = 1;
inline constexpr uint32_t kCreateExclusive = 2;

struct CreateArgs {
  Diropargs where;
  uint32_t mode = kCreateUnchecked;
  backend::SetAttr attrs;      // UNCHECKED/GUARDED
  backend::ExclVerf verf{};    // EXCLUSIVE
  void encode(xdr::XdrEnc& enc) const;
  static Result<CreateArgs> decode(xdr::XdrDec& dec);
};

struct MkdirArgs {
  Diropargs where;
  backend::SetAttr attrs;
  void encode(xdr::XdrEnc& enc) const;
  static Result<MkdirArgs> decode(xdr::XdrDec& dec);
};

struct SymlinkArgs {
  Diropargs where;
  backend::SetAttr attrs;
  std::string target;
  void encode(xdr::XdrEnc& enc) const;
  static Result<SymlinkArgs> decode(xdr::XdrDec& dec);
};

struct MknodArgs {
  Diropargs where;
  backend::FType type = backend::FType::kReg;
  backend::DevT dev{};
  backend::SetAttr attrs;
  static Result<MknodArgs> decode(xdr::XdrDec& dec);
};

struct RenameArgs {
  Diropargs from;
  Diropargs to;
  void encode(xdr::XdrEnc& enc) const;
  static Result<RenameArgs> decode(xdr::XdrDec& dec);
};

struct LinkArgs {
  FileHandle file;
  Diropargs to;
  void encode(xdr::XdrEnc& enc) const;
  static Result<LinkArgs> decode(xdr::XdrDec& dec);
};

struct CommitArgs {
  FileHandle file;
  uint64_t offset = 0;
  uint32_t count = 0;
  void encode(xdr::XdrEnc& enc) const;
  static Result<CommitArgs> decode(xdr::XdrDec& dec);
};

// ---- weak cache consistency -----------------------------------------------

struct WccPre {  // wcc_attr: the pre-op sample of the three CTO-relevant fields
  uint64_t size = 0;
  backend::Timespec mtime{}, ctime{};
};
std::optional<WccPre> wcc_pre(const std::optional<backend::Attr>& attr);
void encode_pre_attr(xdr::XdrEnc& enc, const std::optional<WccPre>& pre);
void encode_wcc(xdr::XdrEnc& enc, const std::optional<WccPre>& pre,
                const std::optional<backend::Attr>& post, uint64_t fsid);

void encode_time(xdr::XdrEnc& enc, const backend::Timespec& time);
void encode_fattr(xdr::XdrEnc& enc, const backend::Attr& attr, uint64_t fsid);
void encode_post_attr(xdr::XdrEnc& enc, const std::optional<backend::Attr>& attr,
                      uint64_t fsid);
void encode_post_fh(xdr::XdrEnc& enc, const std::optional<FileHandle>& fh);

backend::AccessMask access_from_wire(uint32_t value);
uint32_t access_to_wire(backend::AccessMask value);

}  // namespace lnfs::nfsv3
