#include "nfsv3/nfs3_types.hpp"

#include <algorithm>
#include <cerrno>

namespace lnfs::nfsv3 {

Result<FileHandle> FileHandle::decode(xdr::XdrDec& dec) {
  auto value = LNFS_TRY(dec.opaque(kMaxFileHandle));
  if (value.empty()) return Err(Errno::kGarbage);
  return FileHandle{{value.begin(), value.end()}};
}

void Diropargs::encode(xdr::XdrEnc& enc) const {
  dir.encode(enc);
  enc.string(name);
}
Result<Diropargs> Diropargs::decode(xdr::XdrDec& dec) {
  auto dir = LNFS_TRY(FileHandle::decode(dec));
  auto name = LNFS_TRY(dec.string(kMaxName));
  return Diropargs{std::move(dir), std::string(name)};
}

void AccessArgs::encode(xdr::XdrEnc& enc) const {
  object.encode(enc);
  enc.u32(access);
}
Result<AccessArgs> AccessArgs::decode(xdr::XdrDec& dec) {
  AccessArgs out;
  out.object = LNFS_TRY(FileHandle::decode(dec));
  out.access = LNFS_TRY(dec.u32());
  if (out.access & ~kAccessAll) return Err(Errno::kGarbage);
  return out;
}

void ReadArgs::encode(xdr::XdrEnc& enc) const {
  file.encode(enc);
  enc.u64(offset);
  enc.u32(count);
}
Result<ReadArgs> ReadArgs::decode(xdr::XdrDec& dec) {
  ReadArgs out;
  out.file = LNFS_TRY(FileHandle::decode(dec));
  out.offset = LNFS_TRY(dec.u64());
  out.count = LNFS_TRY(dec.u32());
  return out;
}

void ReaddirArgs::encode(xdr::XdrEnc& enc) const {
  dir.encode(enc);
  enc.u64(cookie);
  enc.opaque_fixed(cookieverf);
  enc.u32(count);
}
Result<ReaddirArgs> ReaddirArgs::decode(xdr::XdrDec& dec) {
  ReaddirArgs out;
  out.dir = LNFS_TRY(FileHandle::decode(dec));
  out.cookie = LNFS_TRY(dec.u64());
  auto verf = LNFS_TRY(dec.opaque_fixed(8));
  std::copy(verf.begin(), verf.end(), out.cookieverf.begin());
  out.count = LNFS_TRY(dec.u32());
  return out;
}

void ReaddirPlusArgs::encode(xdr::XdrEnc& enc) const {
  dir.encode(enc);
  enc.u64(cookie);
  enc.opaque_fixed(cookieverf);
  enc.u32(dircount);
  enc.u32(maxcount);
}
Result<ReaddirPlusArgs> ReaddirPlusArgs::decode(xdr::XdrDec& dec) {
  ReaddirPlusArgs out;
  out.dir = LNFS_TRY(FileHandle::decode(dec));
  out.cookie = LNFS_TRY(dec.u64());
  auto verf = LNFS_TRY(dec.opaque_fixed(8));
  std::copy(verf.begin(), verf.end(), out.cookieverf.begin());
  out.dircount = LNFS_TRY(dec.u32());
  out.maxcount = LNFS_TRY(dec.u32());
  return out;
}

void encode_time(xdr::XdrEnc& enc, const backend::Timespec& time) {
  enc.u32(static_cast<uint32_t>(std::clamp<int64_t>(time.sec, 0, UINT32_MAX)));
  enc.u32(std::min(time.nsec, 999999999u));
}

void encode_fattr(xdr::XdrEnc& enc, const backend::Attr& attr, uint64_t fsid) {
  enc.u32(static_cast<uint32_t>(attr.type));
  enc.u32(attr.mode & 07777);
  enc.u32(attr.nlink);
  enc.u32(attr.uid);
  enc.u32(attr.gid);
  enc.u64(attr.size);
  enc.u64(attr.used);
  enc.u32(attr.rdev.major);
  enc.u32(attr.rdev.minor);
  enc.u64(fsid);
  enc.u64(attr.fileid);
  encode_time(enc, attr.atime);
  encode_time(enc, attr.mtime);
  encode_time(enc, attr.ctime);
}

void encode_post_attr(xdr::XdrEnc& enc, const std::optional<backend::Attr>& attr,
                      uint64_t fsid) {
  enc.boolean(attr.has_value());
  if (attr) encode_fattr(enc, *attr, fsid);
}

void encode_post_fh(xdr::XdrEnc& enc, const std::optional<FileHandle>& fh) {
  enc.boolean(fh.has_value());
  if (fh) fh->encode(enc);
}

backend::AccessMask access_from_wire(uint32_t value) {
  backend::AccessMask out;
  if (value & kAccessRead) out.set(backend::Access::kRead);
  if (value & kAccessLookup) out.set(backend::Access::kLookup);
  if (value & kAccessModify) out.set(backend::Access::kModify);
  if (value & kAccessExtend) out.set(backend::Access::kExtend);
  if (value & kAccessDelete) out.set(backend::Access::kDelete);
  if (value & kAccessExecute) out.set(backend::Access::kExecute);
  return out;
}

uint32_t access_to_wire(backend::AccessMask value) {
  uint32_t out = 0;
  if (value.has(backend::Access::kRead)) out |= kAccessRead;
  if (value.has(backend::Access::kLookup)) out |= kAccessLookup;
  if (value.has(backend::Access::kModify)) out |= kAccessModify;
  if (value.has(backend::Access::kExtend)) out |= kAccessExtend;
  if (value.has(backend::Access::kDelete)) out |= kAccessDelete;
  if (value.has(backend::Access::kExecute)) out |= kAccessExecute;
  return out;
}

}  // namespace lnfs::nfsv3
