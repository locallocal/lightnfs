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

Result<backend::SetAttr> decode_sattr(xdr::XdrDec& dec) {
  backend::SetAttr out;
  if (LNFS_TRY(dec.boolean())) out.mode = LNFS_TRY(dec.u32()) & 07777;
  if (LNFS_TRY(dec.boolean())) out.uid = LNFS_TRY(dec.u32());
  if (LNFS_TRY(dec.boolean())) out.gid = LNFS_TRY(dec.u32());
  if (LNFS_TRY(dec.boolean())) out.size = LNFS_TRY(dec.u64());
  auto decode_time_how = [&](backend::SetAttr::TimeHow& how,
                             backend::Timespec& value) -> Result<void> {
    uint32_t wire = LNFS_TRY(dec.u32());
    if (wire > 2) return Err(Errno::kGarbage);
    how = static_cast<backend::SetAttr::TimeHow>(wire);
    if (how == backend::SetAttr::TimeHow::kClient) {
      value.sec = LNFS_TRY(dec.u32());
      value.nsec = LNFS_TRY(dec.u32());
      if (value.nsec > 999999999u) return Err(Errno::kGarbage);
    }
    return {};
  };
  LNFS_TRY(decode_time_how(out.atime_how, out.atime));
  LNFS_TRY(decode_time_how(out.mtime_how, out.mtime));
  return out;
}

void encode_sattr(xdr::XdrEnc& enc, const backend::SetAttr& attrs) {
  auto opt_u32 = [&](const std::optional<uint32_t>& value) {
    enc.boolean(value.has_value());
    if (value) enc.u32(*value);
  };
  opt_u32(attrs.mode);
  opt_u32(attrs.uid);
  opt_u32(attrs.gid);
  enc.boolean(attrs.size.has_value());
  if (attrs.size) enc.u64(*attrs.size);
  auto time_how = [&](backend::SetAttr::TimeHow how, const backend::Timespec& value) {
    enc.u32(static_cast<uint32_t>(how));
    if (how == backend::SetAttr::TimeHow::kClient) {
      enc.u32(static_cast<uint32_t>(value.sec));
      enc.u32(value.nsec);
    }
  };
  time_how(attrs.atime_how, attrs.atime);
  time_how(attrs.mtime_how, attrs.mtime);
}

void SetattrArgs::encode(xdr::XdrEnc& enc) const {
  object.encode(enc);
  encode_sattr(enc, attrs);
  enc.boolean(guard);
  if (guard) {
    enc.u32(static_cast<uint32_t>(guard_ctime.sec));
    enc.u32(guard_ctime.nsec);
  }
}
Result<SetattrArgs> SetattrArgs::decode(xdr::XdrDec& dec) {
  SetattrArgs out;
  out.object = LNFS_TRY(FileHandle::decode(dec));
  out.attrs = LNFS_TRY(decode_sattr(dec));
  out.guard = LNFS_TRY(dec.boolean());
  if (out.guard) {
    out.guard_ctime.sec = LNFS_TRY(dec.u32());
    out.guard_ctime.nsec = LNFS_TRY(dec.u32());
  }
  return out;
}

Result<WriteArgs> WriteArgs::decode(xdr::XdrDec& dec) {
  WriteArgs out;
  out.file = LNFS_TRY(FileHandle::decode(dec));
  out.offset = LNFS_TRY(dec.u64());
  out.count = LNFS_TRY(dec.u32());
  out.stable = LNFS_TRY(dec.u32());
  if (out.stable > kFileSync) return Err(Errno::kGarbage);
  out.data = LNFS_TRY(dec.opaque(1u << 24));
  return out;
}

void CreateArgs::encode(xdr::XdrEnc& enc) const {
  where.encode(enc);
  enc.u32(mode);
  if (mode == kCreateExclusive) enc.opaque_fixed(verf);
  else encode_sattr(enc, attrs);
}
Result<CreateArgs> CreateArgs::decode(xdr::XdrDec& dec) {
  CreateArgs out;
  out.where = LNFS_TRY(Diropargs::decode(dec));
  out.mode = LNFS_TRY(dec.u32());
  if (out.mode > kCreateExclusive) return Err(Errno::kGarbage);
  if (out.mode == kCreateExclusive) {
    auto verf = LNFS_TRY(dec.opaque_fixed(8));
    std::copy(verf.begin(), verf.end(), out.verf.begin());
  } else {
    out.attrs = LNFS_TRY(decode_sattr(dec));
  }
  return out;
}

void MkdirArgs::encode(xdr::XdrEnc& enc) const {
  where.encode(enc);
  encode_sattr(enc, attrs);
}
Result<MkdirArgs> MkdirArgs::decode(xdr::XdrDec& dec) {
  MkdirArgs out;
  out.where = LNFS_TRY(Diropargs::decode(dec));
  out.attrs = LNFS_TRY(decode_sattr(dec));
  return out;
}

void SymlinkArgs::encode(xdr::XdrEnc& enc) const {
  where.encode(enc);
  encode_sattr(enc, attrs);
  enc.string(target);
}
Result<SymlinkArgs> SymlinkArgs::decode(xdr::XdrDec& dec) {
  SymlinkArgs out;
  out.where = LNFS_TRY(Diropargs::decode(dec));
  out.attrs = LNFS_TRY(decode_sattr(dec));
  out.target = std::string(LNFS_TRY(dec.string(kMaxPath)));
  return out;
}

Result<MknodArgs> MknodArgs::decode(xdr::XdrDec& dec) {
  MknodArgs out;
  out.where = LNFS_TRY(Diropargs::decode(dec));
  uint32_t type = LNFS_TRY(dec.u32());
  if (type < 1 || type > 7) return Err(Errno::kGarbage);
  out.type = static_cast<backend::FType>(type);
  if (out.type == backend::FType::kChr || out.type == backend::FType::kBlk) {
    out.attrs = LNFS_TRY(decode_sattr(dec));
    out.dev.major = LNFS_TRY(dec.u32());
    out.dev.minor = LNFS_TRY(dec.u32());
  } else if (out.type == backend::FType::kSock || out.type == backend::FType::kFifo) {
    out.attrs = LNFS_TRY(decode_sattr(dec));
  }
  // REG/DIR/LNK carry no body; the engine answers BADTYPE.
  return out;
}

void RenameArgs::encode(xdr::XdrEnc& enc) const {
  from.encode(enc);
  to.encode(enc);
}
Result<RenameArgs> RenameArgs::decode(xdr::XdrDec& dec) {
  RenameArgs out;
  out.from = LNFS_TRY(Diropargs::decode(dec));
  out.to = LNFS_TRY(Diropargs::decode(dec));
  return out;
}

void LinkArgs::encode(xdr::XdrEnc& enc) const {
  file.encode(enc);
  to.encode(enc);
}
Result<LinkArgs> LinkArgs::decode(xdr::XdrDec& dec) {
  LinkArgs out;
  out.file = LNFS_TRY(FileHandle::decode(dec));
  out.to = LNFS_TRY(Diropargs::decode(dec));
  return out;
}

void CommitArgs::encode(xdr::XdrEnc& enc) const {
  file.encode(enc);
  enc.u64(offset);
  enc.u32(count);
}
Result<CommitArgs> CommitArgs::decode(xdr::XdrDec& dec) {
  CommitArgs out;
  out.file = LNFS_TRY(FileHandle::decode(dec));
  out.offset = LNFS_TRY(dec.u64());
  out.count = LNFS_TRY(dec.u32());
  return out;
}

std::optional<WccPre> wcc_pre(const std::optional<backend::Attr>& attr) {
  if (!attr) return std::nullopt;
  return WccPre{attr->size, attr->mtime, attr->ctime};
}

void encode_pre_attr(xdr::XdrEnc& enc, const std::optional<WccPre>& pre) {
  enc.boolean(pre.has_value());
  if (!pre) return;
  enc.u64(pre->size);
  encode_time(enc, pre->mtime);
  encode_time(enc, pre->ctime);
}

void encode_wcc(xdr::XdrEnc& enc, const std::optional<WccPre>& pre,
                const std::optional<backend::Attr>& post, uint64_t fsid) {
  encode_pre_attr(enc, pre);
  encode_post_attr(enc, post, fsid);
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
