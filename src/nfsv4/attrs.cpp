#include "nfsv4/attrs.hpp"

#include <cassert>
#include <cstring>
#include <span>
#include <string>

namespace lnfs::nfsv4 {

using namespace attr;

namespace {

Bitmap base_supported_attrs() {
    Bitmap b;
    for (uint32_t id : {kSupportedAttrs,
                        kType,
                        kFhExpireType,
                        kChange,
                        kSize,
                        kLinkSupport,
                        kSymlinkSupport,
                        kNamedAttr,
                        kFsid,
                        kUniqueHandles,
                        kLeaseTime,
                        kRdattrError,
                        kCansettime,
                        kCaseInsensitive,
                        kCasePreserving,
                        kChownRestricted,
                        kFilehandle,
                        kFileid,
                        kFilesAvail,
                        kFilesFree,
                        kFilesTotal,
                        kHomogeneous,
                        kMaxfilesize,
                        kMaxlink,
                        kMaxname,
                        kMaxread,
                        kMaxwrite,
                        kMode,
                        kNoTrunc,
                        kNumlinks,
                        kOwner,
                        kOwnerGroup,
                        kRawdev,
                        kSpaceAvail,
                        kSpaceFree,
                        kSpaceTotal,
                        kSpaceUsed,
                        kTimeAccess,
                        kTimeDelta,
                        kTimeMetadata,
                        kTimeModify,
                        kMountedOnFileid,
                        kTimeAccessSet,
                        kTimeModifySet,
                        kSuppattrExclCreat,
                        kChangeAttrType})
        b.set(id);
    return b;
}

// pathname4: component4<>.
void encode_pathname(xdr::XdrEnc& enc, std::span<const std::string> parts) {
    enc.u32(static_cast<uint32_t>(parts.size()));
    for (const auto& part : parts) enc.string(part);
}

// The one location an export has: its owner's host (empty when there is none to name).
std::string location_server(const AttrSource& src) {
    return src.owner && !src.owner->address.empty() ? core::address_host(src.owner->address) : std::string();
}

}  // namespace

const Bitmap& supported_attrs(bool referrals) {
    static const Bitmap plain = base_supported_attrs();
    static const Bitmap with_referrals = [] {
        Bitmap b = base_supported_attrs();
        b.set(kFsLocations);
        b.set(kFsLocationsInfo);
        return b;
    }();
    return referrals ? with_referrals : plain;
}

const Bitmap& write_only_attrs() {
    static const Bitmap value = [] {
        Bitmap b;
        for (uint32_t id : {kTimeAccessSet, kTimeModifySet}) b.set(id);
        return b;
    }();
    return value;
}

bool wants_write_only(const Bitmap& wanted) {
    for (uint32_t id : {kTimeAccessSet, kTimeModifySet})
        if (wanted.test(id)) return true;
    return false;
}

bool wants_stats(const Bitmap& wanted) {
    for (uint32_t id : {kFilesAvail, kFilesFree, kFilesTotal, kSpaceAvail, kSpaceFree, kSpaceTotal})
        if (wanted.test(id)) return true;
    return false;
}

namespace {
void patch_be32(std::byte* gap, uint32_t v) {
    v = xdr::to_be32(v);
    std::memcpy(gap, &v, 4);
}
}  // namespace

void encode_fattr(xdr::XdrEnc& enc, const Bitmap& wanted, const AttrSource& src) {
    Bitmap actual;
    const Bitmap& sup = supported_attrs(src.referrals);
    // Write-only attributes are dropped here as well as rejected by the read paths: an
    // attrmask bit with no value behind it makes attrmask and attrlist disagree, which no
    // client can parse (followups/protocol-gaps.md A1).  One filter, two places, so a new
    // read path cannot reintroduce it.
    const Bitmap& wo = write_only_attrs();
    auto word = [](const Bitmap& b, uint32_t w) { return w < b.words.size() ? b.words[w] : 0u; };
    for (uint32_t w = 0; w < 3; ++w) {
        uint32_t bits = word(wanted, w) & word(sup, w) & ~word(wo, w);
        if (bits) {
            while (actual.words.size() <= w) actual.words.push_back(0);
            actual.words[w] = bits;
        }
    }

    const backend::Attr& a = *src.attr;
    // defaults double as the pseudo-fs answer
    static const core::FsProps kPseudoProps;
    const core::FsProps& fs = src.fs ? *src.fs : kPseudoProps;
    const backend::FsLimits& lim = fs.limits;
    backend::FsStats zero_stats;
    const backend::FsStats& st = src.stats ? *src.stats : zero_stats;

    // attrlist4 body, ascending attribute order, encoded in place: the opaque length is
    // a 4-byte gap patched once the values are down (every value is 4-aligned, so the
    // opaque needs no tail padding).
    actual.encode(enc);
    std::byte* len_gap = enc.raw_gap(4);
    const size_t vals_start = enc.size();
    xdr::XdrEnc& vals = enc;
    // Every attribute below is visited through ok(), in source order, whether or not its
    // bit is set — which makes this the one place that can hold the ascending order
    // fattr4 requires.  A block moved to the wrong place trips the assert on the first
    // encode instead of shipping an attrlist the client parses into the wrong fields
    // (followups/protocol-gaps.md A2: fs_locations_info(67) sat ahead of
    // mounted_on_fileid(55) and only showed up when a client asked for both).
    int64_t last_id = -1;
    auto ok = [&](uint32_t id) {
        assert(static_cast<int64_t>(id) > last_id && "encode_fattr: attributes must be encoded in ascending order");
        last_id = static_cast<int64_t>(id);
        return actual.test(id);
    };

    if (ok(kSupportedAttrs)) supported_attrs(src.referrals).encode(vals);
    if (ok(kType)) vals.u32(static_cast<uint32_t>(a.type));
    // fh_expire_type: FH4_PERSISTENT only when the backend really has persistent handles.
    // Claiming it for a backend without kStableHandles told the client it need not be
    // prepared to recover from an expired handle, while a restart invalidated every one of
    // them (followups/protocol-gaps.md B5).  The synthesized tree (null fs) is persistent:
    // its node ids are path hashes.
    if (ok(kFhExpireType)) vals.u32(!src.fs || src.fs->stable_handles ? kFhPersistent : kFhVolatileAny);
    if (ok(kChange)) vals.u64(a.change);
    if (ok(kSize)) vals.u64(a.size);
    if (ok(kLinkSupport)) vals.boolean(fs.link_support);
    if (ok(kSymlinkSupport)) vals.boolean(fs.symlink_support);
    if (ok(kNamedAttr)) vals.boolean(false);
    if (ok(kFsid)) {
        vals.u64(src.fsid);
        vals.u64(0);
    }
    if (ok(kUniqueHandles)) vals.boolean(true);
    if (ok(kLeaseTime)) vals.u32(src.lease_seconds);
    if (ok(kRdattrError)) vals.u32(src.rdattr_error);
    if (ok(kCansettime)) vals.boolean(core::FsProps::kCansettime);
    if (ok(kCaseInsensitive)) vals.boolean(fs.case_insensitive);
    if (ok(kCasePreserving)) vals.boolean(core::FsProps::kCasePreserving);
    if (ok(kChownRestricted)) vals.boolean(core::FsProps::kChownRestricted);
    if (ok(kFilehandle)) vals.opaque(src.fh);
    if (ok(kFileid)) vals.u64(a.fileid);
    if (ok(kFilesAvail)) vals.u64(st.afiles);
    if (ok(kFilesFree)) vals.u64(st.ffiles);
    if (ok(kFilesTotal)) vals.u64(st.tfiles);
    // fs_locations (RFC 8881 §11.10, plan 12 B2): fs_root = the export's pseudo path;
    // locations = its owner, with the same root path there (every gateway of the
    // cluster presents the same pseudo tree).  The pseudo fs itself, or an export with
    // nobody to name, answers an empty list.
    if (ok(kFsLocations)) {
        encode_pathname(vals, src.fs_root);
        std::string server = location_server(src);
        vals.u32(server.empty() ? 0 : 1);
        if (!server.empty()) {
            // server<>
            vals.u32(1);
            vals.string(server);
            encode_pathname(vals, src.fs_root);
        }
    }
    if (ok(kHomogeneous)) vals.boolean(core::FsProps::kHomogeneous);
    if (ok(kMaxfilesize)) vals.u64(lim.max_filesize);
    if (ok(kMaxlink)) vals.u32(lim.max_link);
    if (ok(kMaxname)) vals.u32(lim.max_name);
    if (ok(kMaxread)) vals.u64(lim.max_read);
    if (ok(kMaxwrite)) vals.u64(lim.max_write);
    if (ok(kMode)) vals.u32(a.mode & 07777);
    if (ok(kNoTrunc)) vals.boolean(core::FsProps::kNoTrunc);
    if (ok(kNumlinks)) vals.u32(a.nlink);
    // AUTH_SYS numeric string
    if (ok(kOwner)) vals.string(std::to_string(a.uid));
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
    if (ok(kMountedOnFileid)) vals.u64(src.mounted_on_fileid ? src.mounted_on_fileid : a.fileid);
    // fs_locations_info (RFC 8881 §11.10.1): the same single location — currency
    // unknown (-1), no fls_info, valid for one lease — under fli_flags 0.
    if (ok(kFsLocationsInfo)) {
        // fli_flags
        vals.u32(0);
        // fli_valid_for
        vals.u32(src.lease_seconds);
        encode_pathname(vals, src.fs_root);
        std::string server = location_server(src);
        // fli_items<>
        vals.u32(server.empty() ? 0 : 1);
        if (!server.empty()) {
            // fli_entries<>
            vals.u32(1);
            // fls_currency = -1
            vals.u32(0xffffffffu);
            // fls_info: empty
            vals.u32(0);
            vals.string(server);
            encode_pathname(vals, src.fs_root);
        }
    }
    if (ok(kSuppattrExclCreat)) settable_attrs().encode(vals);
    // change_attr_type (RFC 7862 §12.2.3): the kNativeChange consumer (plan doc 10
    // §5.3).  A storage version counter is MONOTONIC_INCR; the ctime synthesis of
    // design 05 §5.6 is TIME_METADATA; the pseudo-fs change is the boot epoch, which
    // only ever grows.
    if (ok(kChangeAttrType))
        vals.u32(!src.fs            ? kChangeTypeMonotonicIncr
                 : fs.native_change ? kChangeTypeMonotonicIncr
                                    : kChangeTypeTimeMetadata);

    patch_be32(len_gap, static_cast<uint32_t>(enc.size() - vals_start));
}

const Bitmap& settable_attrs() {
    static const Bitmap value = [] {
        Bitmap b;
        for (uint32_t id : {kSize, kMode, kOwner, kOwnerGroup, kTimeAccessSet, kTimeModifySet}) b.set(id);
        return b;
    }();
    return value;
}

namespace {

// "1000" → 1000.  "name@domain" forms need idmap, which AUTH_SYS deployments do not
// run: BADOWNER sends the client to its numeric fallback (nfs4_disable_idmapping).
bool parse_numeric_owner(std::string_view s, uint32_t& out) {
    if (s.empty() || s.size() > 10) return false;
    uint64_t n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + static_cast<uint64_t>(c - '0');
    }
    if (n > UINT32_MAX) return false;
    out = static_cast<uint32_t>(n);
    return true;
}

}  // namespace

Status decode_settable_fattr(xdr::XdrDec& dec, backend::SetAttr& out, Bitmap& set) {
    auto mask = Bitmap::decode(dec);
    auto vals = mask ? dec.opaque(1u << 20) : Result<std::span<const std::byte>>(Err(Errno::kGarbage));
    if (!mask || !vals) return Status::kBadxdr;
    // Bits above the words we know are ignored on a read but must not be ignored here:
    // silently not setting an attribute the client asked for is worse than refusing it.
    if (mask->beyond_known) return Status::kAttrnotsupp;
    xdr::XdrDec v(*vals);
    const Bitmap& sup = supported_attrs();
    const Bitmap& settable = settable_attrs();
    for (uint32_t bit = 0; bit < 96; ++bit) {
        if (!mask->test(bit)) continue;
        if (!settable.test(bit)) return sup.test(bit) ? Status::kInval : Status::kAttrnotsupp;
        switch (bit) {
            case kSize: {
                auto n = v.u64();
                if (!n) return Status::kBadxdr;
                out.size = *n;
                break;
            }
            case kMode: {
                auto n = v.u32();
                if (!n) return Status::kBadxdr;
                if (*n & ~07777u) return Status::kInval;
                out.mode = *n;
                break;
            }
            case kOwner:
            case kOwnerGroup: {
                auto s = v.string(1024);
                if (!s) return Status::kBadxdr;
                uint32_t id;
                if (!parse_numeric_owner(*s, id)) return Status::kBadowner;
                if (bit == kOwner)
                    out.uid = id;
                else
                    out.gid = id;
                break;
            }
            case kTimeAccessSet:
            case kTimeModifySet: {
                auto how = v.u32();
                if (!how || *how > 1) return Status::kBadxdr;
                backend::Timespec t{};
                // SET_TO_CLIENT_TIME4
                if (*how == 1) {
                    auto sec = v.u64();
                    auto nsec = v.u32();
                    if (!sec || !nsec) return Status::kBadxdr;
                    if (*nsec >= 1000000000u) return Status::kInval;
                    t.sec = static_cast<int64_t>(*sec);
                    t.nsec = *nsec;
                }
                auto mode = *how == 1 ? backend::SetAttr::TimeHow::kClient : backend::SetAttr::TimeHow::kServer;
                if (bit == kTimeAccessSet) {
                    out.atime_how = mode;
                    out.atime = t;
                } else {
                    out.mtime_how = mode;
                    out.mtime = t;
                }
                break;
            }
            default:
                return Status::kAttrnotsupp;
        }
        set.set(bit);
    }
    // trailing bytes: malformed attrlist
    if (!v.at_end()) return Status::kBadxdr;
    return Status::kOk;
}

}  // namespace lnfs::nfsv4
