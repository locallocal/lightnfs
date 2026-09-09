#pragma once
// v4 bitmap attribute layer (design 04 §4.5, nfsv4 research 03/11.5): one table drives
// GETATTR and READDIR.  Minimal honest set: the 13 REQUIRED attributes plus what Linux
// clients actually consume (mode/owner/times/limits/space/numlinks/mounted_on_fileid).
// owner/owner_group are decimal strings (AUTH_SYS convention, zero idmap dependency).

#include "backend/api.hpp"
#include "core/fs_owner_view.hpp"
#include "core/fs_props.hpp"
#include "nfsv4/nfs4_types.hpp"

namespace lnfs::nfsv4 {

namespace attr {
inline constexpr uint32_t kSupportedAttrs = 0, kType = 1, kFhExpireType = 2, kChange = 3, kSize = 4, kLinkSupport = 5,
                          kSymlinkSupport = 6, kNamedAttr = 7, kFsid = 8, kUniqueHandles = 9, kLeaseTime = 10,
                          kRdattrError = 11, kCansettime = 15, kCaseInsensitive = 16, kCasePreserving = 17,
                          kChownRestricted = 18, kFilehandle = 19, kFileid = 20, kFilesAvail = 21, kFilesFree = 22,
                          kFilesTotal = 23, kHomogeneous = 26, kMaxfilesize = 27, kMaxlink = 28, kMaxname = 29,
                          kMaxread = 30, kMaxwrite = 31, kMode = 33, kNoTrunc = 34, kNumlinks = 35, kOwner = 36,
                          kOwnerGroup = 37, kRawdev = 41, kSpaceAvail = 42, kSpaceFree = 43, kSpaceTotal = 44,
                          kSpaceUsed = 45, kTimeAccess = 47, kTimeAccessSet = 48, kTimeDelta = 51, kTimeMetadata = 52,
                          kTimeModify = 53, kTimeModifySet = 54, kMountedOnFileid = 55, kSuppattrExclCreat = 75,
                          kChangeAttrType = 79;
// Referral attributes (RFC 8881 §11.10, plan 12 B2): supported under active-active only.
inline constexpr uint32_t kFsLocations = 24, kFsLocationsInfo = 67;
// change_attr_type values (RFC 7862 §12.2.3).
inline constexpr uint32_t kChangeTypeMonotonicIncr = 0, kChangeTypeVersionCounter = 1,
                          kChangeTypeVersionCounterNoPnfs = 2, kChangeTypeTimeMetadata = 3, kChangeTypeUndefined = 4;
}  // namespace attr

// `referrals` (the engine's active-active flag, plan 12 B1/B2) adds fs_locations and
// fs_locations_info to the set.
const Bitmap& supported_attrs(bool referrals = false);

// Everything the encoders may need; the engine prefetches async pieces (stats) first.
struct AttrSource {
    const backend::Attr* attr = nullptr;
    uint64_t fsid = 0;                        // fsid4.major; 0 = pseudo-fs
    uint64_t mounted_on_fileid = 0;           // defaults to attr->fileid when 0
    std::span<const std::byte> fh{};          // attr 19 (filehandle)
    const core::FsProps* fs = nullptr;        // caps/limits derivation; null = pseudo
    const backend::FsStats* stats = nullptr;  // null -> zeros (pseudo)
    uint32_t lease_seconds = kLeaseSeconds;   // attr 10 (lease_time)
    // Referrals (plan 12 B2): `referrals` selects the supported set; for an export-side
    // object `fs_root` is the export's pseudo path and `owner` its owner as this gateway
    // sees it (null: no location to name — the pseudo fs, or an unknown owner).
    bool referrals = false;
    std::span<const std::string> fs_root{};
    const core::FsOwner* owner = nullptr;
    // attr 11: non-zero when attributes were left out of the answer (an absent export
    // answers NFS4ERR_MOVED here, RFC 8881 §11.11.1 / §18.23).
    uint32_t rdattr_error = 0;
};

// Decodes a fattr4 carrying settable attributes (SETATTR / OPEN create / CREATE) into
// a backend SetAttr.  `set` receives the bits actually applied.  Returns kOk, kBadxdr,
// kInval (read-only attribute requested), kAttrnotsupp (unsupported settable attribute,
// e.g. ACL) or kBadowner (non-numeric owner string; AUTH_SYS convention).
Status decode_settable_fattr(xdr::XdrDec& dec, backend::SetAttr& out, Bitmap& set);

// The settable subset of supported_attrs() (size/mode/owner/owner_group/time_*_set).
const Bitmap& settable_attrs();

// True if the mask requests attrs that need a statfs() prefetch.
bool wants_stats(const Bitmap& wanted);

// Encodes fattr4 {attrmask, attr_vals} for wanted ∩ supported into enc. Values are
// encoded in place behind a patched length gap — no staging buffer (plan doc 10 §2.4).
void encode_fattr(xdr::XdrEnc& enc, const Bitmap& wanted, const AttrSource& src);

}  // namespace lnfs::nfsv4
