#include "mini_test.hpp"

#include <arpa/inet.h>

#include <array>

#include "backend/fault.hpp"
#include "backend/memory/memory.hpp"
#include "core/config.hpp"
#include "core/errmap.hpp"
#include "core/file_handle.hpp"
#include "core/obj_lock.hpp"
#include "mountd/mount3.hpp"
#include "nfsv3/engine.hpp"
#include "rpc/drc.hpp"
#include "runtime/reactor.hpp"
#include "runtime/testing/fake_ring.hpp"
#include "transport/connection.hpp"

using namespace lnfs;

namespace {

struct NfsFixture {
    rt::testing::FakeRing ring;
    rt::Reactor reactor{ring};
    rt::BufferPool pool;
    transport::TransportConfig transport_cfg;
    transport::ConnCtx ctx{5, peer(), pool, transport_cfg};
    core::ExportTable exports;
    backend::MemoryBackend* memory = nullptr;
    std::array<std::byte, 16> key{};
    core::FileHandleCodec handles;
    core::ObjLockRegistry locks;
    nfsv3::Engine engine;
    mountd::Mount3 mount;
    rpc::Dispatcher dispatcher;
    nfsv3::FileHandle root_fh;

    static transport::Peer peer() {
        transport::Peer p;
        auto* addr = reinterpret_cast<sockaddr_in*>(&p.addr);
        addr->sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &addr->sin_addr);
        p.len = sizeof(*addr);
        return p;
    }

    NfsFixture()
        : handles(core::FileHandleCodec::from_key(key)), engine(exports, handles, locks), mount(exports, handles) {
        auto mem = std::make_unique<backend::MemoryBackend>(23);
        memory = mem.get();
        (void)memory->add_dir("/d");
        (void)memory->add_file("/hello", "hello world");
        (void)memory->add_file("/d/a", "a");
        (void)memory->add_symlink("/link", "hello");
        core::ExportConfig cfg;
        cfg.path = "/export";
        cfg.fsid = 23;
        cfg.clients = {"127.0.0.0/8"};
        (void)exports.add(cfg, std::move(mem));
        engine.register_with(dispatcher);
        mount.register_with(dispatcher);
        std::optional<Result<backend::ObjPtr>> root;
        rt::spawn(
            [](backend::MemoryBackend* backend, std::optional<Result<backend::ObjPtr>>* out) -> rt::Task<void> {
                out->emplace(co_await backend->root());
            }(memory, &root),
            reactor);
        while (!root) reactor.poll_once();
        root_fh.data = handles.encode(*exports.by_fsid(23), (**root)->id());
    }

    rt::BufferChain call(uint32_t xid, uint32_t proc, rt::BufferChain args = {}, uint32_t program = nfsv3::kProgram) {
        xdr::XdrEnc enc(pool);
        enc.u32(xid);
        enc.u32(rpc::kCall);
        enc.u32(2);
        enc.u32(program);
        enc.u32(nfsv3::kVersion);
        enc.u32(proc);
        // AUTH_NONE
        enc.u32(0);
        enc.u32(0);
        enc.u32(0);
        enc.u32(0);
        if (!args.empty()) enc.opaque_fixed(args.to_bytes());
        return enc.take();
    }

    std::vector<std::byte> request(uint32_t proc, rt::BufferChain args = {}) {
        rt::spawn(dispatcher.handle_request(ctx, call(0x1234, proc, std::move(args))), reactor);
        while (!ring.has_pending(rt::testing::FakeRing::Kind::kSendv)) reactor.poll_once();
        auto op = ring.take(rt::testing::FakeRing::Kind::kSendv, 5);
        std::vector<std::byte> wire;
        for (int i = 0; i < op.iovcnt; ++i) {
            auto* p = static_cast<std::byte*>(op.iov[i].iov_base);
            wire.insert(wire.end(), p, p + op.iov[i].iov_len);
        }
        ring.complete(op, wire.size());
        while (reactor.poll_once()) {
        }
        return {wire.begin() + 4, wire.end()};
    }

    std::vector<std::byte> mount_request(uint32_t proc, rt::BufferChain args = {}) {
        rt::spawn(dispatcher.handle_request(ctx, call(0x4321, proc, std::move(args), mountd::kProgram)), reactor);
        while (!ring.has_pending(rt::testing::FakeRing::Kind::kSendv)) reactor.poll_once();
        auto op = ring.take(rt::testing::FakeRing::Kind::kSendv, 5);
        std::vector<std::byte> wire;
        for (int i = 0; i < op.iovcnt; ++i) {
            auto* p = static_cast<std::byte*>(op.iov[i].iov_base);
            wire.insert(wire.end(), p, p + op.iov[i].iov_len);
        }
        ring.complete(op, static_cast<int32_t>(wire.size()));
        while (reactor.poll_once()) {
        }
        return {wire.begin() + 4, wire.end()};
    }

    xdr::XdrDec result(std::vector<std::byte>& bytes) {
        xdr::XdrDec dec(std::span<const std::byte>(bytes.data(), bytes.size()));
        // xid
        (void)dec.u32();
        // reply
        (void)dec.u32();
        // accepted
        (void)dec.u32();
        // verf flavor
        (void)dec.u32();
        // verf len
        (void)dec.u32();
        // RPC success
        (void)dec.u32();
        return dec;
    }
};

// The RPC accept_stat, so a GARBAGE_ARGS reply is distinguishable from a real one.
uint32_t accept_stat(const std::vector<std::byte>& bytes) {
    xdr::XdrDec dec(std::span<const std::byte>(bytes.data(), bytes.size()));
    for (int i = 0; i < 5; ++i) (void)dec.u32();
    auto v = dec.u32();
    return v ? *v : ~0u;
}

// The NFS/MOUNT status word, or ~0 when the reply carries no body at all (an accept-error
// reply such as GARBAGE_ARGS has none).  Reading it blindly aborts in Result::value(),
// which turns a test failure into a crash -- and these tests exist precisely to check
// what happens on the inputs that used to produce a bodyless reply.
uint32_t reply_status(NfsFixture& f, std::vector<std::byte>& bytes) {
    auto dec = f.result(bytes);
    auto v = dec.u32();
    return v ? *v : ~0u;
}

}  // namespace

TEST(Nfs3Types, ReadAndReaddirArgsRoundTrip) {
    rt::BufferPool pool;
    nfsv3::ReadArgs read{{{std::byte{1}, std::byte{2}}}, 99, 4096};
    xdr::XdrEnc enc(pool);
    read.encode(enc);
    auto chain = enc.take();
    xdr::XdrDec dec(chain);
    auto decoded = nfsv3::ReadArgs::decode(dec);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->offset, 99u);
    EXPECT_EQ(decoded->count, 4096u);
    EXPECT_EQ(decoded->file.data.size(), 2u);

    nfsv3::ReaddirPlusArgs plus{{{std::byte{3}}}, 7, {}, 1024, 8192};
    xdr::XdrEnc plus_enc(pool);
    plus.encode(plus_enc);
    auto plus_chain = plus_enc.take();
    xdr::XdrDec plus_dec(plus_chain);
    auto plus_out = nfsv3::ReaddirPlusArgs::decode(plus_dec);
    ASSERT_TRUE(plus_out.has_value());
    EXPECT_EQ(plus_out->cookie, 7u);
    EXPECT_EQ(plus_out->dircount, 1024u);
    EXPECT_EQ(plus_out->maxcount, 8192u);
}

TEST(Nfs3, GetattrLookupReadAndReaddirPlusWireFlow) {
    NfsFixture f;
    xdr::XdrEnc getarg(f.pool);
    f.root_fh.encode(getarg);
    auto reply = f.request((uint32_t)nfsv3::Proc::kGetattr, getarg.take());
    auto result = f.result(reply);
    EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kOk);
    EXPECT_EQ(*result.u32(), (uint32_t)backend::FType::kDir);

    xdr::XdrEnc lookarg(f.pool);
    nfsv3::Diropargs{f.root_fh, "hello"}.encode(lookarg);
    reply = f.request((uint32_t)nfsv3::Proc::kLookup, lookarg.take());
    result = f.result(reply);
    EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kOk);
    auto file_fh = *result.opaque(64);
    nfsv3::FileHandle file{{file_fh.begin(), file_fh.end()}};

    xdr::XdrEnc readarg(f.pool);
    nfsv3::ReadArgs{file, 0, 64}.encode(readarg);
    reply = f.request((uint32_t)nfsv3::Proc::kRead, readarg.take());
    result = f.result(reply);
    EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kOk);
    // post-op attrs follow
    EXPECT_TRUE(*result.boolean());
    // fattr3 = 84 bytes
    for (int i = 0; i < 21; ++i) (void)result.u32();
    EXPECT_EQ(*result.u32(), 11u);
    EXPECT_TRUE(*result.boolean());
    auto data = *result.opaque(64);
    EXPECT_STREQ(std::string(reinterpret_cast<const char*>(data.data()), data.size()), "hello world");

    xdr::XdrEnc rdarg(f.pool);
    nfsv3::ReaddirPlusArgs{f.root_fh, 0, {}, 4096, 16384}.encode(rdarg);
    reply = f.request((uint32_t)nfsv3::Proc::kReaddirplus, rdarg.take());
    result = f.result(reply);
    EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kOk);
    EXPECT_TRUE(*result.boolean());
}

TEST(Nfs3, ErrorWhitelistFiltersInvalidMappings) {
    EXPECT_EQ((uint32_t)core::to_v3(errno_from(ENOENT), nfsv3::Proc::kLookup), (uint32_t)nfsv3::Status::kNoent);
    EXPECT_EQ((uint32_t)core::to_v3(errno_from(ENOSPC), nfsv3::Proc::kGetattr), (uint32_t)nfsv3::Status::kIo);
    EXPECT_EQ((uint32_t)core::to_v3(Errno::kBadHandle, nfsv3::Proc::kRead), (uint32_t)nfsv3::Status::kBadhandle);
}

TEST(Nfs3, ReaddirRejectsMismatchedCookieVerifier) {
    NfsFixture f;
    std::array<std::byte, 8> verifier{};
    // cannot collide with a logical-clock change attr
    verifier[7] = std::byte{0xFF};
    xdr::XdrEnc args(f.pool);
    nfsv3::ReaddirArgs{f.root_fh, 3, verifier, 4096}.encode(args);
    auto reply = f.request((uint32_t)nfsv3::Proc::kReaddir, args.take());
    auto result = f.result(reply);
    EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kBadCookie);
}

// followups/protocol-gaps.md A3: filename3 / nfspath3 are string<> on the wire, so a
// name or path longer than the filesystem will take has to come back as
// NFS3ERR_NAMETOOLONG (the client's ENAMETOOLONG), not as an RPC-level GARBAGE_ARGS that
// the client can only turn into EIO.  C5 rides along: an unusable component in LOOKUP is
// answered too, not rejected at the RPC layer.
// followups/protocol-gaps.md B6: FSINFO's `properties` never carried FSF3_CANSETTIME,
// although v3 SETATTR does accept SET_TO_CLIENT_TIME and the v4 cansettime attribute
// already said so.  Clients that read the bit (BSD / Solaris / macOS) fall back to
// SET_TO_SERVER_TIME without it and lose the timestamps utimes() / tar -p are restoring.
TEST(Nfs3, FsinfoAdvertisesCanSetTime) {
    NfsFixture f;
    xdr::XdrEnc args(f.pool);
    f.root_fh.encode(args);
    auto reply = f.request((uint32_t)nfsv3::Proc::kFsinfo, args.take());
    ASSERT_TRUE(reply_status(f, reply) == (uint32_t)nfsv3::Status::kOk);
    auto dec = f.result(reply);
    (void)dec.u32();
    // post_op_attr
    ASSERT_TRUE(*dec.boolean());
    for (int i = 0; i < 21; ++i) (void)dec.u32();
    // rtmax, rtpref, rtmult, wtmax, wtpref, wtmult, dtpref
    for (int i = 0; i < 7; ++i) ASSERT_TRUE(dec.u32().has_value());
    // maxfilesize
    ASSERT_TRUE(dec.u64().has_value());
    // time_delta: seconds + nseconds
    ASSERT_TRUE(dec.u32().has_value());
    ASSERT_TRUE(dec.u32().has_value());
    auto props = dec.u32();
    ASSERT_TRUE(props.has_value());
    EXPECT_TRUE((*props & nfsv3::kFsfCanSetTime) != 0);
    // The memory backend has both, and the server is homogeneous by construction: the new
    // bit must be added to those, not replace them.
    EXPECT_TRUE((*props & nfsv3::kFsfLink) != 0);
    EXPECT_TRUE((*props & nfsv3::kFsfSymlink) != 0);
    EXPECT_TRUE((*props & nfsv3::kFsfHomogeneous) != 0);
    // Nothing outside the four bits RFC 1813 §3.3.19 defines.
    EXPECT_EQ(*props & ~(nfsv3::kFsfLink | nfsv3::kFsfSymlink | nfsv3::kFsfHomogeneous | nfsv3::kFsfCanSetTime), 0u);

    // The bit is honest: sattr3 carrying SET_TO_CLIENT_TIME really does decode into a
    // client-time SetAttr for the engine to hand down.  Asserted at the decoder rather
    // than over the wire on purpose -- a real SETATTR here answers PERM, because the
    // fixture's anonymous credential does not own the file and POSIX utimes() with
    // explicit times needs ownership.  That it reaches the backend when the caller is
    // entitled to is covered by WriteTypes.SattrAndCreateRoundTrip and
    // Nfs4.SetattrSizeModeOwner.
    nfsv3::SetattrArgs sa;
    sa.object = f.root_fh;
    sa.attrs.mtime_how = backend::SetAttr::TimeHow::kClient;
    sa.attrs.mtime = {1234567, 89};
    xdr::XdrEnc sargs(f.pool);
    sa.encode(sargs);
    auto flat = sargs.take().to_bytes();
    xdr::XdrDec sdec(std::span<const std::byte>(flat.data(), flat.size()));
    auto parsed = nfsv3::SetattrArgs::decode(sdec);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->attrs.mtime_how == backend::SetAttr::TimeHow::kClient);
    EXPECT_EQ(parsed->attrs.mtime.sec, 1234567);
    EXPECT_EQ(parsed->attrs.mtime.nsec, 89u);
}

TEST(Nfs3, OverlongNamesAnswerNametoolong) {
    NfsFixture f;
    const auto kNametoolong = (uint32_t)nfsv3::Status::kNametoolong;
    const std::string long_name(256, 'x');
    const std::string ok_name(255, 'y');

    auto dirop = [&](nfsv3::Proc proc, const std::string& name) {
        xdr::XdrEnc args(f.pool);
        nfsv3::Diropargs{f.root_fh, name}.encode(args);
        return f.request(static_cast<uint32_t>(proc), args.take());
    };
    // LOOKUP: was GARBAGE_ARGS, now a real reply carrying NAMETOOLONG.
    auto reply = dirop(nfsv3::Proc::kLookup, long_name);
    EXPECT_EQ(accept_stat(reply), rpc::kSuccess);
    EXPECT_EQ(reply_status(f, reply), kNametoolong);
    // 255 is the memory backend's max_name: accepted as a name, simply not present.
    reply = dirop(nfsv3::Proc::kLookup, ok_name);
    EXPECT_EQ(reply_status(f, reply), (uint32_t)nfsv3::Status::kNoent);
    // C5: a component that can never exist is a lookup miss, not an RPC error.
    for (const std::string& bad : {std::string("a/b"), std::string("")}) {
        reply = dirop(nfsv3::Proc::kLookup, bad);
        EXPECT_EQ(accept_stat(reply), rpc::kSuccess);
        EXPECT_EQ(reply_status(f, reply), (uint32_t)nfsv3::Status::kNoent);
    }
    // REMOVE / RMDIR take the same path through MutateGuard::precheck.
    for (nfsv3::Proc proc : {nfsv3::Proc::kRemove, nfsv3::Proc::kRmdir}) {
        reply = dirop(proc, long_name);
        EXPECT_EQ(accept_stat(reply), rpc::kSuccess);
        EXPECT_EQ(reply_status(f, reply), kNametoolong);
    }
    // RMDIR still distinguishes "." and ".." (RFC 1813 §3.3.13) -- the length arm must
    // not have swallowed the dot arm.
    reply = dirop(nfsv3::Proc::kRmdir, ".");
    EXPECT_EQ(reply_status(f, reply), (uint32_t)nfsv3::Status::kInval);
    reply = dirop(nfsv3::Proc::kRmdir, "..");
    EXPECT_EQ(reply_status(f, reply), (uint32_t)nfsv3::Status::kExist);

    // The creation family: CREATE / MKDIR / SYMLINK.
    xdr::XdrEnc create(f.pool);
    nfsv3::CreateArgs cargs;
    cargs.where = {f.root_fh, long_name};
    cargs.mode = nfsv3::kCreateUnchecked;
    cargs.encode(create);
    reply = f.request((uint32_t)nfsv3::Proc::kCreate, create.take());
    EXPECT_EQ(reply_status(f, reply), kNametoolong);

    xdr::XdrEnc mkdir(f.pool);
    nfsv3::MkdirArgs{{f.root_fh, long_name}, {}}.encode(mkdir);
    reply = f.request((uint32_t)nfsv3::Proc::kMkdir, mkdir.take());
    EXPECT_EQ(reply_status(f, reply), kNametoolong);

    xdr::XdrEnc symlink(f.pool);
    nfsv3::SymlinkArgs{{f.root_fh, long_name}, {}, "target"}.encode(symlink);
    reply = f.request((uint32_t)nfsv3::Proc::kSymlink, symlink.take());
    EXPECT_EQ(reply_status(f, reply), kNametoolong);

    // RENAME / LINK check both names through the same guard.
    xdr::XdrEnc rename(f.pool);
    nfsv3::RenameArgs{{f.root_fh, "hello"}, {f.root_fh, long_name}}.encode(rename);
    reply = f.request((uint32_t)nfsv3::Proc::kRename, rename.take());
    EXPECT_EQ(reply_status(f, reply), kNametoolong);

    xdr::XdrEnc link(f.pool);
    nfsv3::LinkArgs{f.root_fh, {f.root_fh, long_name}}.encode(link);
    reply = f.request((uint32_t)nfsv3::Proc::kLink, link.take());
    EXPECT_EQ(reply_status(f, reply), kNametoolong);

    // Past the decode ceiling a request no longer names anything a filesystem could
    // hold: GARBAGE_ARGS stays the answer there.
    reply = dirop(nfsv3::Proc::kLookup, std::string(nfsv3::kNameWireMax + 1, 'z'));
    EXPECT_EQ(accept_stat(reply), rpc::kGarbageArgs);
}

// A3, the functional half: a symlink target between MNTPATHLEN(1024) and PATH_MAX(4096)
// is a perfectly legal POSIX target and used to be rejected at the decoder.
TEST(Nfs3, LongSymlinkTargetIsAccepted) {
    NfsFixture f;
    const std::string target(2000, 't');
    xdr::XdrEnc args(f.pool);
    nfsv3::SymlinkArgs{{f.root_fh, "long-link"}, {}, target}.encode(args);
    auto reply = f.request((uint32_t)nfsv3::Proc::kSymlink, args.take());
    ASSERT_TRUE(reply_status(f, reply) == (uint32_t)nfsv3::Status::kOk);
    auto result = f.result(reply);
    (void)result.u32();
    // post_op_fh3, then READLINK it back.
    ASSERT_TRUE(*result.boolean());
    auto fh_bytes = *result.opaque(64);
    nfsv3::FileHandle link{{fh_bytes.begin(), fh_bytes.end()}};
    xdr::XdrEnc rl(f.pool);
    link.encode(rl);
    reply = f.request((uint32_t)nfsv3::Proc::kReadlink, rl.take());
    ASSERT_TRUE(reply_status(f, reply) == (uint32_t)nfsv3::Status::kOk);
    result = f.result(reply);
    (void)result.u32();
    // post-op attrs
    ASSERT_TRUE(*result.boolean());
    for (int i = 0; i < 21; ++i) (void)result.u32();
    auto got = result.string(nfsv3::kPathWireMax);
    ASSERT_TRUE(got.has_value());
    EXPECT_STREQ(std::string(*got), target);

    // Beyond PATH_MAX the storage would refuse it anyway: NAMETOOLONG, not GARBAGE_ARGS.
    xdr::XdrEnc too_long(f.pool);
    nfsv3::SymlinkArgs{{f.root_fh, "too-long"}, {}, std::string(nfsv3::kMaxPath + 1, 't')}.encode(too_long);
    reply = f.request((uint32_t)nfsv3::Proc::kSymlink, too_long.take());
    EXPECT_EQ(accept_stat(reply), rpc::kSuccess);
    EXPECT_EQ(reply_status(f, reply), (uint32_t)nfsv3::Status::kNametoolong);
}

// A3, MOUNT side: MNT3ERR_NAMETOOLONG exists for exactly this.
// followups/protocol-gaps.md B8: a mount point is a directory.  MNT used to hand back the
// filehandle of whatever the path named, so mounting a path ending at a regular file
// succeeded and then failed every lookup underneath with nothing saying why.
TEST(Mount3, MntRefusesANonDirectory) {
    NfsFixture f;
    // /export/hello is a regular file in the fixture's backing tree.
    xdr::XdrEnc file_arg(f.pool);
    file_arg.string("/export/hello");
    auto reply = f.mount_request(1, file_arg.take());
    EXPECT_EQ(accept_stat(reply), rpc::kSuccess);
    // MNT3ERR_NOTDIR
    EXPECT_EQ(reply_status(f, reply), 20u);

    // A symlink is not a directory either.
    xdr::XdrEnc link_arg(f.pool);
    link_arg.string("/export/link");
    auto link_reply = f.mount_request(1, link_arg.take());
    EXPECT_EQ(reply_status(f, link_reply), 20u);

    // The export root and a directory inside it still mount.
    for (const char* path : {"/export", "/export/d"}) {
        xdr::XdrEnc dir_arg(f.pool);
        dir_arg.string(path);
        auto ok = f.mount_request(1, dir_arg.take());
        EXPECT_EQ(reply_status(f, ok), 0u);
    }
}

TEST(Mount3, OverlongPathAnswersNametoolong) {
    NfsFixture f;
    xdr::XdrEnc args(f.pool);
    args.string(std::string(mountd::kMaxMountPath + 1, '/'));
    auto reply = f.mount_request(1, args.take());
    EXPECT_EQ(accept_stat(reply), rpc::kSuccess);
    EXPECT_EQ(reply_status(f, reply), 63u);
    // An over-long component inside an existing export answers the same way.
    xdr::XdrEnc deep(f.pool);
    deep.string("/export/" + std::string(256, 'x'));
    reply = f.mount_request(1, deep.take());
    EXPECT_EQ(reply_status(f, reply), 63u);
}

TEST(Mount3, MountAndExportWireFlow) {
    NfsFixture f;
    xdr::XdrEnc args(f.pool);
    args.string("/export");
    auto reply = f.mount_request(1, args.take());
    auto result = f.result(reply);
    EXPECT_EQ(*result.u32(), 0u);
    auto fh = result.opaque(64);
    ASSERT_TRUE(fh.has_value());
    EXPECT_TRUE(!fh->empty());
    EXPECT_EQ(*result.u32(), 1u);
    // AUTH_SYS
    EXPECT_EQ(*result.u32(), 1u);

    reply = f.mount_request(5);
    result = f.result(reply);
    EXPECT_TRUE(*result.boolean());
    EXPECT_STREQ(std::string(*result.string(1024)), "/export");
}

// Plan 12 C1: a gateway may run with no export at all (catalog mode before the first
// catalog).  MOUNT lists nothing and mounts nothing; an old handle is stale.
TEST(Mount3, EmptyExportSetServesNothing) {
    NfsFixture f;
    core::ExportSetPlan plan;
    plan.remove.push_back(23);
    std::vector<std::unique_ptr<backend::Backend>> none;
    ASSERT_TRUE(f.exports.apply(std::move(plan), none, 1).has_value());
    EXPECT_EQ(f.exports.size(), 0u);

    xdr::XdrEnc args(f.pool);
    args.string("/export");
    auto reply = f.mount_request(1, args.take());
    auto result = f.result(reply);
    // MNT3ERR_ACCES
    EXPECT_EQ(*result.u32(), 13u);
    reply = f.mount_request(5);
    result = f.result(reply);
    // no exports
    EXPECT_FALSE(*result.boolean());
    // NFS GETATTR on the handle minted before the removal: STALE.
    xdr::XdrEnc getattr(f.pool);
    getattr.opaque(f.root_fh.data);
    reply = f.request(static_cast<uint32_t>(nfsv3::Proc::kGetattr), getattr.take());
    result = f.result(reply);
    EXPECT_EQ(*result.u32(), static_cast<uint32_t>(nfsv3::Status::kStale));
}

// Development plan §9 "错误映射" row: the v3 whitelist is checked against the research
// table itself (docs/reference/nfsv3/08-errors.md §8.2), regenerated by scripts/gen_errmap_cases.py.
// Every status the document lists for a procedure must pass v3_error_allowed, and the
// universal rows (IO/SERVERFAULT everywhere, STALE/BADHANDLE for handle-taking procs).
TEST(Nfs3, ErrorWhitelistMatchesResearchTable) {
    using nfsv3::Proc;
    using nfsv3::Status;
    struct Case {
        Proc proc;
        Status status;
        const char* proc_name;
        const char* status_name;
    };
    static const Case kCases[] = {
#include "errmap_v3_cases.inc"
    };
    for (const auto& c : kCases) {
        if (!core::v3_error_allowed(c.proc, c.status))
            MT_FAIL("doc lists %s for %s but v3_error_allowed rejects it", c.status_name, c.proc_name);
    }
    for (uint32_t p = 1; p <= 21; ++p) {
        auto proc = static_cast<Proc>(p);
        EXPECT_TRUE(core::v3_error_allowed(proc, Status::kIo));
        EXPECT_TRUE(core::v3_error_allowed(proc, Status::kServerfault));
        EXPECT_TRUE(core::v3_error_allowed(proc, Status::kStale));
        EXPECT_TRUE(core::v3_error_allowed(proc, Status::kBadhandle));
    }
    EXPECT_FALSE(core::v3_error_allowed(Proc::kNull, Status::kStale));
}

// Semantic cookie verifier (plan doc 10 §5.1): the reply verifier round-trips while
// the directory is unchanged; a modification invalidates the old pair (BAD_COOKIE)
// and the client restarts from cookie 0.
TEST(Nfs3, ReaddirVerifierRoundTripAndChangeDetection) {
    NfsFixture f;
    auto list = [&](uint64_t cookie, std::array<std::byte, 8> verf, uint32_t* status, uint64_t* last_cookie,
                    std::array<std::byte, 8>* out_verf) {
        xdr::XdrEnc args(f.pool);
        nfsv3::ReaddirArgs{f.root_fh, cookie, verf, 8192}.encode(args);
        auto reply = f.request((uint32_t)nfsv3::Proc::kReaddir, args.take());
        auto result = f.result(reply);
        *status = *result.u32();
        if (*status != 0) return;
        // post_op_attr
        if (*result.boolean()) (void)result.skip(84);
        auto got = *result.opaque_fixed(8);
        std::copy(got.begin(), got.end(), out_verf->begin());
        while (*result.boolean()) {
            // fileid
            (void)result.u64();
            (void)result.string(255);
            *last_cookie = *result.u64();
        }
    };

    uint32_t status = 1;
    uint64_t last_cookie = 0;
    std::array<std::byte, 8> verf{};
    list(0, {}, &status, &last_cookie, &verf);
    ASSERT_TRUE(status == 0);
    ASSERT_TRUE(last_cookie != 0);

    uint64_t ignore = 0;
    std::array<std::byte, 8> verf2{};
    list(last_cookie, verf, &status, &ignore, &verf2);
    // unchanged directory: pair accepted
    EXPECT_EQ(status, 0u);

    ASSERT_TRUE(f.memory->add_file("/added-mid-listing", "x").has_value());
    list(last_cookie, verf, &status, &ignore, &verf2);
    EXPECT_EQ(status, (uint32_t)nfsv3::Status::kBadCookie);

    // restart recovers with a fresh verifier
    list(0, {}, &status, &last_cookie, &verf2);
    EXPECT_EQ(status, 0u);
    EXPECT_FALSE(verf2 == verf);
}

// kJukebox end to end (plan doc 10 §5.3): a backend "try again later" reaches the
// wire as NFS3ERR_JUKEBOX on READ (one of the two procedures the 08 §8.2 whitelist
// admits it on; both are idempotent and never DRC-cached, so a retransmission always
// re-executes) and the retry succeeds once the backend is ready.
// followups/protocol-gaps.md C2: the strict cookie verifier buys "no duplicated and no
// missing entries" at the price of restarting a listing whenever the directory changes.
// On a directory under continuous churn that restart can keep happening, so an operator
// needs (a) a way to see it and (b) a way to trade it away per export.
TEST(Nfs3, ReaddirCookieToleranceAndMetric) {
    NfsFixture f;
    auto list = [&](uint64_t cookie, std::array<std::byte, 8> verf, uint32_t* status, uint64_t* last_cookie,
                    std::array<std::byte, 8>* out_verf) {
        xdr::XdrEnc args(f.pool);
        nfsv3::ReaddirArgs{f.root_fh, cookie, verf, 8192}.encode(args);
        auto reply = f.request((uint32_t)nfsv3::Proc::kReaddir, args.take());
        auto result = f.result(reply);
        *status = *result.u32();
        if (*status != 0) return;
        if (*result.boolean()) (void)result.skip(84);
        auto got = *result.opaque_fixed(8);
        std::copy(got.begin(), got.end(), out_verf->begin());
        while (*result.boolean()) {
            (void)result.u64();
            (void)result.string(255);
            *last_cookie = *result.u64();
        }
    };

    // Strict is the default, and a rejected listing is now countable: the per-procedure
    // error counter cannot tell BAD_COOKIE from NOTDIR or TOOSMALL.
    uint64_t before = obs::Metrics::instance().v3_bad_cookie.load(std::memory_order_relaxed);
    uint32_t status = 1;
    uint64_t last_cookie = 0, ignore = 0;
    std::array<std::byte, 8> verf{}, verf2{};
    list(0, {}, &status, &last_cookie, &verf);
    ASSERT_TRUE(status == 0);
    ASSERT_TRUE(last_cookie != 0);
    ASSERT_TRUE(f.memory->add_file("/churn-1", "x").has_value());
    list(last_cookie, verf, &status, &ignore, &verf2);
    ASSERT_TRUE(status == (uint32_t)nfsv3::Status::kBadCookie);
    EXPECT_EQ(obs::Metrics::instance().v3_bad_cookie.load(std::memory_order_relaxed), before + 1);

    // Tolerant: a zero verifier, never checked, so the same stale pair keeps listing.
    // That is knfsd's behaviour and what POSIX readdir() already allows.
    core::ExportSetPlan plan;
    auto cfg = f.exports.snapshot()->by_fsid(23);
    ASSERT_TRUE(cfg != nullptr);
    core::ExportConfig relaxed;
    relaxed.path = "/export";
    relaxed.fsid = 23;
    relaxed.clients = {"127.0.0.0/8"};
    relaxed.squash = core::Squash::kNone;
    relaxed.strict_readdir_cookies = false;
    plan.update.push_back(relaxed);
    std::vector<std::unique_ptr<backend::Backend>> none;
    ASSERT_TRUE(f.exports.apply(std::move(plan), none, f.exports.snapshot()->epoch).has_value());
    EXPECT_FALSE(f.exports.snapshot()->by_fsid(23)->strict_readdir_cookies.load());

    uint64_t after_switch = obs::Metrics::instance().v3_bad_cookie.load(std::memory_order_relaxed);
    std::array<std::byte, 8> zero{};
    list(0, {}, &status, &last_cookie, &verf);
    ASSERT_TRUE(status == 0);
    // the verifier handed out is all zeroes now
    EXPECT_TRUE(verf == zero);
    ASSERT_TRUE(f.memory->add_file("/churn-2", "y").has_value());
    // The churn no longer invalidates the listing: the old cookie is still accepted.
    list(last_cookie, verf, &status, &ignore, &verf2);
    EXPECT_EQ(status, 0u);
    // ...and a stale non-zero verifier is not rejected either, because nothing is checked.
    std::array<std::byte, 8> bogus{};
    bogus[0] = std::byte{0x5A};
    list(last_cookie, bogus, &status, &ignore, &verf2);
    EXPECT_EQ(status, 0u);
    // Nothing was counted while tolerant.
    EXPECT_EQ(obs::Metrics::instance().v3_bad_cookie.load(std::memory_order_relaxed), after_switch);
}

TEST(Nfs3, JukeboxReachesTheWireAndRetrySucceeds) {
    NfsFixture f;
    rpc::Drc drc({.ttl = std::chrono::milliseconds(60000), .max_memory = 1 << 20});
    f.engine.set_drc(&drc);
    xdr::XdrEnc lookarg(f.pool);
    nfsv3::Diropargs{f.root_fh, "hello"}.encode(lookarg);
    auto reply = f.request((uint32_t)nfsv3::Proc::kLookup, lookarg.take());
    auto result = f.result(reply);
    EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kOk);
    auto file_fh = *result.opaque(64);
    nfsv3::FileHandle file{{file_fh.begin(), file_fh.end()}};

    backend::fault::arm(backend::fault::Kind::kJukebox, 1);
    xdr::XdrEnc readarg(f.pool);
    nfsv3::ReadArgs{file, 0, 64}.encode(readarg);
    reply = f.request((uint32_t)nfsv3::Proc::kRead, readarg.take());
    result = f.result(reply);
    EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kJukebox);
    // post-op attrs still follow (WCC for the retry)
    EXPECT_TRUE(*result.boolean());
    EXPECT_EQ(drc.stats().inserts, 0u);

    // Same xid, same args: re-executed, not replayed.
    xdr::XdrEnc again(f.pool);
    nfsv3::ReadArgs{file, 0, 64}.encode(again);
    reply = f.request((uint32_t)nfsv3::Proc::kRead, again.take());
    result = f.result(reply);
    EXPECT_EQ(*result.u32(), (uint32_t)nfsv3::Status::kOk);
    EXPECT_EQ(drc.stats().replays, 0u);
    backend::fault::clear();
}
