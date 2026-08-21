#include "nfsv4/attrs.hpp"

#include <string>

namespace lnfs::nfsv4 {

using namespace attr;

const Bitmap& supported_attrs() {
  static const Bitmap value = [] {
    Bitmap b;
    for (uint32_t id : {kSupportedAttrs, kType, kFhExpireType, kChange, kSize,
                        kLinkSupport, kSymlinkSupport, kNamedAttr, kFsid, kUniqueHandles,
                        kLeaseTime, kRdattrError, kCansettime, kCaseInsensitive,
                        kCasePreserving, kChownRestricted, kFilehandle, kFileid,
                        kFilesAvail, kFilesFree, kFilesTotal, kHomogeneous, kMaxfilesize,
                        kMaxlink, kMaxname, kMaxread, kMaxwrite, kMode, kNoTrunc,
                        kNumlinks, kOwner, kOwnerGroup, kRawdev, kSpaceAvail, kSpaceFree,
                        kSpaceTotal, kSpaceUsed, kTimeAccess, kTimeDelta, kTimeMetadata,
                        kTimeModify, kMountedOnFileid})
      b.set(id);
    return b;
  }();
  return value;
}

bool wants_stats(const Bitmap& wanted) {
  for (uint32_t id : {kFilesAvail, kFilesFree, kFilesTotal, kSpaceAvail, kSpaceFree,
                      kSpaceTotal})
    if (wanted.test(id)) return true;
  return false;
}

void encode_fattr(xdr::XdrEnc& enc, const Bitmap& wanted, const AttrSource& src,
                  rt::BufferPool& pool) {
  Bitmap actual;
  const Bitmap& sup = supported_attrs();
  for (uint32_t w = 0; w < 3; ++w) {
    uint32_t bits = (w < wanted.words.size() ? wanted.words[w] : 0) &
                    (w < sup.words.size() ? sup.words[w] : 0);
    if (bits) {
      while (actual.words.size() <= w) actual.words.push_back(0);
      actual.words[w] = bits;
    }
  }

  const backend::Attr& a = *src.attr;
  backend::FsLimits defaults;
  const backend::FsLimits& lim = src.limits ? *src.limits : defaults;
  backend::FsStats zero_stats;
  const backend::FsStats& st = src.stats ? *src.stats : zero_stats;

  xdr::XdrEnc vals(pool);  // attrlist4 body, ascending attribute order
  auto ok = [&](uint32_t id) { return actual.test(id); };

  if (ok(kSupportedAttrs)) supported_attrs().encode(vals);
  if (ok(kType)) vals.u32(static_cast<uint32_t>(a.type));
  if (ok(kFhExpireType)) vals.u32(0);  // FH4_PERSISTENT
  if (ok(kChange)) vals.u64(a.change);
  if (ok(kSize)) vals.u64(a.size);
  if (ok(kLinkSupport)) vals.boolean(src.link_support);
  if (ok(kSymlinkSupport)) vals.boolean(src.symlink_support);
  if (ok(kNamedAttr)) vals.boolean(false);
  if (ok(kFsid)) {
    vals.u64(src.fsid);
    vals.u64(0);
  }
  if (ok(kUniqueHandles)) vals.boolean(true);
  if (ok(kLeaseTime)) vals.u32(kLeaseSeconds);
  if (ok(kRdattrError)) vals.u32(0);
  if (ok(kCansettime)) vals.boolean(true);
  if (ok(kCaseInsensitive)) vals.boolean(src.case_insensitive);
  if (ok(kCasePreserving)) vals.boolean(true);
  if (ok(kChownRestricted)) vals.boolean(true);
  if (ok(kFilehandle)) vals.opaque(src.fh);
  if (ok(kFileid)) vals.u64(a.fileid);
  if (ok(kFilesAvail)) vals.u64(st.afiles);
  if (ok(kFilesFree)) vals.u64(st.ffiles);
  if (ok(kFilesTotal)) vals.u64(st.tfiles);
  if (ok(kHomogeneous)) vals.boolean(true);
  if (ok(kMaxfilesize)) vals.u64(lim.max_filesize);
  if (ok(kMaxlink)) vals.u32(lim.max_link);
  if (ok(kMaxname)) vals.u32(lim.max_name);
  if (ok(kMaxread)) vals.u64(lim.max_read);
  if (ok(kMaxwrite)) vals.u64(lim.max_write);
  if (ok(kMode)) vals.u32(a.mode & 07777);
  if (ok(kNoTrunc)) vals.boolean(true);
  if (ok(kNumlinks)) vals.u32(a.nlink);
  if (ok(kOwner)) vals.string(std::to_string(a.uid));       // AUTH_SYS numeric string
  if (ok(kOwnerGroup)) vals.string(std::to_string(a.gid));
  if (ok(kRawdev)) {
    vals.u32(a.rdev.major);
    vals.u32(a.rdev.minor);
  }
  if (ok(kSpaceAvail)) vals.u64(st.abytes);
  if (ok(kSpaceFree)) vals.u64(st.fbytes);
  if (ok(kSpaceTotal)) vals.u64(st.tbytes);
  if (ok(kSpaceUsed)) vals.u64(a.used);
  if (ok(kTimeAccess)) encode_nfstime(vals, a.atime);
  if (ok(kTimeDelta)) encode_nfstime(vals, lim.time_delta);
  if (ok(kTimeMetadata)) encode_nfstime(vals, a.ctime);
  if (ok(kTimeModify)) encode_nfstime(vals, a.mtime);
  if (ok(kMountedOnFileid))
    vals.u64(src.mounted_on_fileid ? src.mounted_on_fileid : a.fileid);

  actual.encode(enc);
  auto bytes = vals.take().to_bytes();
  enc.opaque(bytes);
}

}  // namespace lnfs::nfsv4
