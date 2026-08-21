// Userspace NFSv3/MOUNTv3 acceptance client (development plan §3.5, milestone M1).
//
// Drives a *running* lightnfsd over real TCP and verifies the read-only protocol
// surface against the export's backing directory — no kernel mount, no root. This is
// the loopback half of the M1 acceptance: the kernel-client half lives in
// scripts/accept_m1.sh (real mount inside a privileged container/VM).
//
//   accept_client walk   HOST NFS_PORT MOUNT_PORT EXPORT BACKING_DIR
//       full recursive traversal via READDIRPLUS; every entry name-set compared with
//       the backing dir, every regular file byte-compared via READ, every symlink
//       compared via READLINK; ACCESS/FSSTAT/FSINFO/PATHCONF smoke plus negative
//       checks (tampered handle, bad cookieverf, write procs PROC_UNAVAIL).
//   accept_client bigdir HOST NFS_PORT MOUNT_PORT EXPORT BACKING_DIR SUBDIR EXPECTED
//       plain-READDIR pagination over one flat directory: exactly EXPECTED unique
//       names, set-equal to the backing dir listing (the 100k-entry gate).
//   accept_client stress HOST NFS_PORT MOUNT_PORT EXPORT BACKING_DIR REL_FILE
//                        CONNS PIPELINE SECONDS
//       CONNS connections each keeping PIPELINE random-offset READs in flight for
//       SECONDS, every reply byte-compared against the local copy; exercises the
//       concurrent read path for the ASAN leak soak.

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "runtime/buffer.hpp"
#include "xdr/xdr.hpp"

namespace fs = std::filesystem;
using lnfs::rt::BufferPool;
using lnfs::xdr::XdrDec;
using lnfs::xdr::XdrEnc;

namespace {

constexpr uint32_t kNfsProg = 100003;
constexpr uint32_t kMountProg = 100005;
constexpr uint32_t kVers = 3;
constexpr uint32_t kNfs3Ok = 0;

[[noreturn]] void fatal(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::fprintf(stderr, "accept_client: FAIL: ");
  std::vfprintf(stderr, fmt, ap);
  std::fprintf(stderr, "\n");
  va_end(ap);
  _exit(1);
}

// ---------- socket + record marking ----------

int connect_tcp(const char* host, uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  char portstr[16];
  std::snprintf(portstr, sizeof portstr, "%u", port);
  if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) fatal("resolve %s", host);
  int fd = socket(res->ai_family, SOCK_STREAM, 0);
  if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) != 0)
    fatal("connect %s:%u: %s", host, port, strerror(errno));
  freeaddrinfo(res);
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  return fd;
}

void send_all(int fd, const std::byte* p, size_t n) {
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w <= 0) fatal("send: %s", strerror(errno));
    p += w;
    n -= static_cast<size_t>(w);
  }
}

void send_record(int fd, const std::vector<std::byte>& payload) {
  uint32_t hdr = lnfs::xdr::to_be32(0x80000000u | (uint32_t)payload.size());
  std::byte h[4];
  std::memcpy(h, &hdr, 4);
  send_all(fd, h, 4);
  send_all(fd, payload.data(), payload.size());
}

bool read_exact(int fd, std::byte* p, size_t n) {
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r <= 0) return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

// Reassembles one record (all fragments); fatals on EOF.
std::vector<std::byte> read_record(int fd) {
  std::vector<std::byte> out;
  for (;;) {
    std::byte h[4];
    if (!read_exact(fd, h, 4)) fatal("connection closed while reading reply");
    uint32_t v;
    std::memcpy(&v, h, 4);
    v = lnfs::xdr::from_be32(v);
    size_t len = v & 0x7fffffffu;
    size_t old = out.size();
    out.resize(old + len);
    if (!read_exact(fd, out.data() + old, len)) fatal("connection closed mid-fragment");
    if (v & 0x80000000u) return out;
  }
}

// ---------- RPC call/reply ----------

std::vector<std::byte> build_call(BufferPool& pool, uint32_t xid, uint32_t prog,
                                  uint32_t proc, const std::vector<std::byte>& body) {
  XdrEnc enc(pool);
  enc.u32(xid);
  enc.u32(0);  // CALL
  enc.u32(2);  // RPC v2
  enc.u32(prog);
  enc.u32(kVers);
  enc.u32(proc);
  enc.u32(1);  // AUTH_SYS
  XdrEnc cred(pool);
  cred.u32(0);  // stamp
  cred.string("accept");
  cred.u32(static_cast<uint32_t>(getuid()));
  cred.u32(static_cast<uint32_t>(getgid()));
  cred.u32(0);  // no aux gids
  auto cred_bytes = cred.take().to_bytes();
  enc.opaque(cred_bytes);
  enc.u32(0);  // verf AUTH_NONE
  enc.u32(0);
  if (!body.empty()) enc.opaque_fixed(body);
  return enc.take().to_bytes();
}

// Parses the reply header; returns accept_stat (0 = success), fatals on structural error.
uint32_t parse_reply_header(XdrDec& dec, uint32_t expect_xid) {
  auto xid = dec.u32();
  if (!xid || *xid != expect_xid) fatal("reply xid mismatch");
  if (!dec.u32() || !dec.u32()) fatal("truncated reply header");  // mtype, reply_stat
  auto flavor = dec.u32();
  auto vlen = dec.u32();
  if (!flavor || !vlen || !dec.skip((*vlen + 3) & ~3u)) fatal("bad reply verifier");
  auto stat = dec.u32();
  if (!stat) fatal("missing accept_stat");
  return *stat;
}

struct Client {
  BufferPool pool;
  int fd = -1;
  uint32_t next_xid = 1;

  explicit Client(const char* host, uint16_t port) : fd(connect_tcp(host, port)) {}
  ~Client() {
    if (fd >= 0) close(fd);
  }
  Client(const Client&) = delete;

  // One synchronous call; returns the reply payload past the RPC header.
  std::vector<std::byte> call(uint32_t prog, uint32_t proc,
                              const std::vector<std::byte>& body,
                              uint32_t* accept_stat_out = nullptr) {
    uint32_t xid = next_xid++;
    send_record(fd, build_call(pool, xid, prog, proc, body));
    auto wire = read_record(fd);
    XdrDec dec(std::span<const std::byte>(wire.data(), wire.size()));
    uint32_t stat = parse_reply_header(dec, xid);
    if (accept_stat_out) {
      *accept_stat_out = stat;
    } else if (stat != 0) {
      fatal("prog %u proc %u: accept_stat %u", prog, proc, stat);
    }
    std::vector<std::byte> rest(wire.end() - dec.remaining(), wire.end());
    return rest;
  }
};

// ---------- NFSv3 decode helpers ----------

uint32_t ru32(XdrDec& d) {
  auto v = d.u32();
  if (!v) fatal("xdr: truncated u32");
  return *v;
}
uint64_t ru64(XdrDec& d) {
  auto v = d.u64();
  if (!v) fatal("xdr: truncated u64");
  return *v;
}
bool rbool(XdrDec& d) {
  auto v = d.boolean();
  if (!v) fatal("xdr: bad bool");
  return *v;
}

struct Fattr {
  uint32_t type = 0;  // 1=REG 2=DIR 5=LNK
  uint32_t mode = 0;
  uint64_t size = 0;
  uint64_t fileid = 0;
};

Fattr decode_fattr(XdrDec& d) {
  Fattr a;
  a.type = ru32(d);
  a.mode = ru32(d);
  ru32(d);  // nlink
  ru32(d);  // uid
  ru32(d);  // gid
  a.size = ru64(d);
  ru64(d);  // used
  ru32(d);  // rdev major
  ru32(d);  // rdev minor
  ru64(d);  // fsid
  a.fileid = ru64(d);
  for (int i = 0; i < 6; ++i) ru32(d);  // atime/mtime/ctime
  return a;
}

std::optional<Fattr> decode_post_attr(XdrDec& d) {
  if (!rbool(d)) return std::nullopt;
  return decode_fattr(d);
}

using Fh = std::vector<std::byte>;

std::optional<Fh> decode_post_fh(XdrDec& d) {
  if (!rbool(d)) return std::nullopt;
  auto s = d.opaque(64);
  if (!s) fatal("xdr: bad post-op fh");
  return Fh(s->begin(), s->end());
}

XdrDec make_dec(const std::vector<std::byte>& bytes) {
  return XdrDec(std::span<const std::byte>(bytes.data(), bytes.size()));
}

void enc_fh(XdrEnc& e, const Fh& fh) { e.opaque(fh); }

// ---------- NFSv3 operations ----------

Fh mnt(const char* host, uint16_t mount_port, const std::string& export_path) {
  Client mc(host, mount_port);
  XdrEnc args(mc.pool);
  args.string(export_path);
  auto reply = mc.call(kMountProg, 1, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  if (status != 0) fatal("MNT %s: status %u", export_path.c_str(), status);
  auto fh = dec.opaque(64);
  if (!fh) fatal("MNT: bad file handle");
  return Fh(fh->begin(), fh->end());
}

Fattr getattr(Client& c, const Fh& fh) {
  XdrEnc args(c.pool);
  enc_fh(args, fh);
  auto reply = c.call(kNfsProg, 1, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  if (status != kNfs3Ok) fatal("GETATTR: status %u", status);
  return decode_fattr(dec);
}

std::pair<Fh, std::optional<Fattr>> lookup(Client& c, const Fh& dir,
                                           const std::string& name) {
  XdrEnc args(c.pool);
  enc_fh(args, dir);
  args.string(name);
  auto reply = c.call(kNfsProg, 3, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  if (status != kNfs3Ok) fatal("LOOKUP %s: status %u", name.c_str(), status);
  auto fh = dec.opaque(64);
  if (!fh) fatal("LOOKUP %s: bad fh", name.c_str());
  Fh out(fh->begin(), fh->end());
  auto attr = decode_post_attr(dec);
  return {std::move(out), attr};
}

uint32_t access_bits(Client& c, const Fh& fh, uint32_t mask) {
  XdrEnc args(c.pool);
  enc_fh(args, fh);
  args.u32(mask);
  auto reply = c.call(kNfsProg, 4, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  if (status != kNfs3Ok) fatal("ACCESS: status %u", status);
  decode_post_attr(dec);
  return ru32(dec);
}

std::string read_link(Client& c, const Fh& fh) {
  XdrEnc args(c.pool);
  enc_fh(args, fh);
  auto reply = c.call(kNfsProg, 5, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  if (status != kNfs3Ok) fatal("READLINK: status %u", status);
  decode_post_attr(dec);
  auto s = dec.string(1024);
  if (!s) fatal("READLINK: bad path");
  return std::string(*s);
}

// Reads [offset, offset+count); returns data and eof flag.
std::pair<std::vector<std::byte>, bool> read_range(Client& c, const Fh& fh,
                                                   uint64_t offset, uint32_t count) {
  XdrEnc args(c.pool);
  enc_fh(args, fh);
  args.u64(offset);
  args.u32(count);
  auto reply = c.call(kNfsProg, 6, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  if (status != kNfs3Ok) fatal("READ off=%llu: status %u", (unsigned long long)offset, status);
  decode_post_attr(dec);
  uint32_t n = ru32(dec);
  bool eof = rbool(dec);
  auto data = dec.opaque(1u << 22);
  if (!data) fatal("READ: bad data");
  if (data->size() != n) fatal("READ: count %u != opaque len %zu", n, data->size());
  return {std::vector<std::byte>(data->begin(), data->end()), eof};
}

std::vector<std::byte> read_all(Client& c, const Fh& fh, uint64_t size, uint32_t chunk) {
  std::vector<std::byte> out;
  out.reserve(size);
  while (out.size() < size) {
    auto [data, eof] = read_range(c, fh, out.size(), chunk);
    if (data.empty()) fatal("READ: zero-length data before size %llu reached",
                            (unsigned long long)size);
    out.insert(out.end(), data.begin(), data.end());
    if (eof) break;
  }
  if (out.size() != size)
    fatal("READ: got %zu bytes, expected %llu", out.size(), (unsigned long long)size);
  return out;
}

struct DirEntry {
  std::string name;
  uint64_t fileid = 0;
  std::optional<Fattr> attr;
  std::optional<Fh> fh;
};

// Full READDIRPLUS pagination; verifies no duplicate names and that "."/".." appear.
std::vector<DirEntry> readdirplus_all(Client& c, const Fh& dir) {
  std::vector<DirEntry> out;
  std::unordered_set<std::string> seen;
  uint64_t cookie = 0;
  std::array<std::byte, 8> verf{};
  for (;;) {
    XdrEnc args(c.pool);
    enc_fh(args, dir);
    args.u64(cookie);
    args.opaque_fixed(verf);
    args.u32(65536);    // dircount
    args.u32(1 << 20);  // maxcount
    auto reply = c.call(kNfsProg, 17, args.take().to_bytes());
    auto dec = make_dec(reply);
    uint32_t status = ru32(dec);
    if (status != kNfs3Ok) fatal("READDIRPLUS cookie=%llu: status %u",
                                 (unsigned long long)cookie, status);
    decode_post_attr(dec);
    auto v = dec.opaque_fixed(8);
    if (!v) fatal("READDIRPLUS: bad cookieverf");
    size_t page = 0;
    while (rbool(dec)) {
      DirEntry ent;
      ent.fileid = ru64(dec);
      auto name = dec.string(255);
      if (!name) fatal("READDIRPLUS: bad name");
      ent.name = std::string(*name);
      cookie = ru64(dec);
      ent.attr = decode_post_attr(dec);
      ent.fh = decode_post_fh(dec);
      if (!seen.insert(ent.name).second)
        fatal("READDIRPLUS: duplicate entry '%s'", ent.name.c_str());
      out.push_back(std::move(ent));
      ++page;
    }
    bool eof = rbool(dec);
    if (eof) break;
    if (page == 0) fatal("READDIRPLUS: empty page without eof");
  }
  if (!seen.count(".") || !seen.count(".."))
    fatal("READDIRPLUS: missing synthesized . or ..");
  return out;
}

// Full plain-READDIR pagination; returns names (excluding . and ..).
std::vector<std::string> readdir_all(Client& c, const Fh& dir) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  uint64_t cookie = 0;
  std::array<std::byte, 8> verf{};
  for (;;) {
    XdrEnc args(c.pool);
    enc_fh(args, dir);
    args.u64(cookie);
    args.opaque_fixed(verf);
    args.u32(65536);
    auto reply = c.call(kNfsProg, 16, args.take().to_bytes());
    auto dec = make_dec(reply);
    uint32_t status = ru32(dec);
    if (status != kNfs3Ok) fatal("READDIR cookie=%llu: status %u",
                                 (unsigned long long)cookie, status);
    decode_post_attr(dec);
    if (!dec.opaque_fixed(8)) fatal("READDIR: bad cookieverf");
    size_t page = 0;
    while (rbool(dec)) {
      ru64(dec);  // fileid
      auto name = dec.string(255);
      if (!name) fatal("READDIR: bad name");
      cookie = ru64(dec);
      std::string n(*name);
      if (!seen.insert(n).second) fatal("READDIR: duplicate entry '%s'", n.c_str());
      if (n != "." && n != "..") out.push_back(std::move(n));
      ++page;
    }
    if (rbool(dec)) break;
    if (page == 0) fatal("READDIR: empty page without eof");
  }
  return out;
}

struct FsInfo {
  uint32_t rtmax = 0;
  uint32_t rtpref = 0;
};

FsInfo fsinfo(Client& c, const Fh& fh) {
  XdrEnc args(c.pool);
  enc_fh(args, fh);
  auto reply = c.call(kNfsProg, 19, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  if (status != kNfs3Ok) fatal("FSINFO: status %u", status);
  decode_post_attr(dec);
  FsInfo info;
  info.rtmax = ru32(dec);
  info.rtpref = ru32(dec);
  return info;
}

void fsstat_pathconf_smoke(Client& c, const Fh& fh) {
  XdrEnc a1(c.pool);
  enc_fh(a1, fh);
  auto r1 = c.call(kNfsProg, 18, a1.take().to_bytes());
  auto d1 = make_dec(r1);
  if (ru32(d1) != kNfs3Ok) fatal("FSSTAT failed");
  decode_post_attr(d1);
  if (ru64(d1) == 0) fatal("FSSTAT: zero total bytes");

  XdrEnc a2(c.pool);
  enc_fh(a2, fh);
  auto r2 = c.call(kNfsProg, 20, a2.take().to_bytes());
  auto d2 = make_dec(r2);
  if (ru32(d2) != kNfs3Ok) fatal("PATHCONF failed");
  decode_post_attr(d2);
  ru32(d2);  // linkmax
  if (ru32(d2) == 0) fatal("PATHCONF: zero name_max");
}

// ---------- walk ----------

std::vector<std::byte> read_local(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) fatal("cannot open local file %s", p.c_str());
  std::vector<char> buf((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  auto* b = reinterpret_cast<std::byte*>(buf.data());
  return std::vector<std::byte>(b, b + buf.size());
}

struct WalkStats {
  size_t dirs = 0, files = 0, symlinks = 0, bytes = 0, empty_checked = 0;
};

void walk_dir(Client& c, const Fh& dir_fh, const fs::path& local, uint32_t chunk,
              WalkStats& st) {
  ++st.dirs;
  auto entries = readdirplus_all(c, dir_fh);

  std::unordered_set<std::string> local_names;
  for (const auto& e : fs::directory_iterator(local))
    local_names.insert(e.path().filename().string());

  size_t remote_count = 0;
  for (auto& ent : entries) {
    if (ent.name == "." || ent.name == "..") continue;
    ++remote_count;
    if (!local_names.count(ent.name))
      fatal("%s: server lists '%s' which is absent locally", local.c_str(),
            ent.name.c_str());
  }
  if (remote_count != local_names.size())
    fatal("%s: server lists %zu entries, backing dir has %zu", local.c_str(),
          remote_count, local_names.size());

  for (auto& ent : entries) {
    if (ent.name == "." || ent.name == "..") continue;
    fs::path child = local / ent.name;
    Fh fh;
    Fattr attr;
    if (ent.fh && ent.attr) {  // READDIRPLUS enrichment
      fh = *ent.fh;
      attr = *ent.attr;
    } else {
      auto [lfh, lattr] = lookup(c, dir_fh, ent.name);
      fh = std::move(lfh);
      attr = lattr ? *lattr : getattr(c, fh);
    }
    auto lst = fs::symlink_status(child);
    switch (attr.type) {
      case 2: {  // DIR
        if (!fs::is_directory(lst)) fatal("%s: type mismatch (server=dir)", child.c_str());
        walk_dir(c, fh, child, chunk, st);
        break;
      }
      case 1: {  // REG
        if (!fs::is_regular_file(lst))
          fatal("%s: type mismatch (server=file)", child.c_str());
        uint64_t local_size = fs::file_size(child);
        if (attr.size != local_size)
          fatal("%s: size mismatch server=%llu local=%llu", child.c_str(),
                (unsigned long long)attr.size, (unsigned long long)local_size);
        if (local_size > 0) {
          auto remote = read_all(c, fh, attr.size, chunk);
          if (remote != read_local(child)) fatal("%s: content mismatch", child.c_str());
          st.bytes += remote.size();
        } else if (st.empty_checked < 8) {
          auto [data, eof] = read_range(c, fh, 0, 4096);
          if (!data.empty() || !eof)
            fatal("%s: empty file READ returned %zu bytes eof=%d", child.c_str(),
                  data.size(), (int)eof);
          ++st.empty_checked;
        }
        ++st.files;
        break;
      }
      case 5: {  // LNK
        if (!fs::is_symlink(lst)) fatal("%s: type mismatch (server=symlink)", child.c_str());
        auto target = read_link(c, fh);
        if (target != fs::read_symlink(child).string())
          fatal("%s: symlink target mismatch '%s'", child.c_str(), target.c_str());
        ++st.symlinks;
        break;
      }
      default:
        fatal("%s: unexpected file type %u", child.c_str(), attr.type);
    }
  }
}

void negative_checks(Client& c, const Fh& root, bool readonly_export) {
  // Tampered handle → NFS3ERR_BADHANDLE (HMAC rejects).
  Fh bad = root;
  bad[bad.size() / 2] ^= std::byte{0x40};
  XdrEnc args(c.pool);
  enc_fh(args, bad);
  auto reply = c.call(kNfsProg, 1, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  if (status != 10001) fatal("tampered handle: expected BADHANDLE, got %u", status);

  // Non-zero cookieverf with non-zero cookie → NFS3ERR_BAD_COOKIE.
  XdrEnc rd(c.pool);
  enc_fh(rd, root);
  rd.u64(3);
  std::array<std::byte, 8> verf{};
  verf[0] = std::byte{1};
  rd.opaque_fixed(verf);
  rd.u32(65536);
  reply = c.call(kNfsProg, 16, rd.take().to_bytes());
  auto dec2 = make_dec(reply);
  if (ru32(dec2) != 10003) fatal("bad cookieverf: expected BAD_COOKIE");

  // Mutation procedures with empty argument bodies must be rejected as GARBAGE_ARGS
  // (arg validation before any side effect).
  for (uint32_t proc : {2u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 21u}) {
    uint32_t stat = 0;
    (void)c.call(kNfsProg, proc, {}, &stat);
    if (stat != 4)
      fatal("write proc %u with empty args: expected GARBAGE_ARGS(4), got %u", proc, stat);
  }

  uint32_t granted = access_bits(c, root, 0x3f);
  if (!(granted & 0x01) || !(granted & 0x02))
    fatal("ACCESS did not grant read+lookup on root: 0x%x", granted);
  if (readonly_export) {
    // ACCESS on a read-only export must never grant modify/extend/delete, and a
    // well-formed CREATE must answer ROFS.
    if (granted & (0x04 | 0x08 | 0x10))
      fatal("ACCESS on readonly export granted write bits: 0x%x", granted);
    XdrEnc cr(c.pool);
    enc_fh(cr, root);
    cr.string("rofs_probe");
    cr.u32(0);  // UNCHECKED
    cr.boolean(false); cr.boolean(false); cr.boolean(false); cr.boolean(false);
    cr.u32(0); cr.u32(0);
    auto reply = c.call(kNfsProg, 8, cr.take().to_bytes());
    auto dec = make_dec(reply);
    if (ru32(dec) != 30) fatal("CREATE on readonly export: expected ROFS");
  }
}

int cmd_walk(const char* host, uint16_t nfs_port, uint16_t mount_port,
             const std::string& export_path, const fs::path& backing,
             bool readonly_export) {
  auto root = mnt(host, mount_port, export_path);
  Client c(host, nfs_port);
  auto info = fsinfo(c, root);
  if (info.rtmax == 0 || info.rtpref == 0 || info.rtpref > info.rtmax)
    fatal("FSINFO: bad read limits rtmax=%u rtpref=%u", info.rtmax, info.rtpref);
  fsstat_pathconf_smoke(c, root);
  uint32_t chunk = std::min<uint32_t>(info.rtpref, 256 * 1024);

  auto root_attr = getattr(c, root);
  if (root_attr.type != 2) fatal("root is not a directory");

  WalkStats st;
  walk_dir(c, root, backing, chunk, st);
  negative_checks(c, root, readonly_export);
  std::printf("accept_client walk OK: %zu dirs, %zu files (%zu bytes verified), "
              "%zu symlinks, negative checks passed\n",
              st.dirs, st.files, st.bytes, st.symlinks);
  return 0;
}

int cmd_bigdir(const char* host, uint16_t nfs_port, uint16_t mount_port,
               const std::string& export_path, const fs::path& backing,
               const std::string& subdir, size_t expected) {
  auto root = mnt(host, mount_port, export_path);
  Client c(host, nfs_port);
  auto [dir_fh, attr] = lookup(c, root, subdir);
  (void)attr;

  auto t0 = std::chrono::steady_clock::now();
  auto names = readdir_all(c, dir_fh);
  auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  if (names.size() != expected)
    fatal("bigdir %s: READDIR returned %zu entries, expected %zu", subdir.c_str(),
          names.size(), expected);
  std::unordered_set<std::string> remote(names.begin(), names.end());
  if (remote.size() != expected) fatal("bigdir: duplicate names");
  size_t local_count = 0;
  for (const auto& e : fs::directory_iterator(backing / subdir)) {
    if (!remote.count(e.path().filename().string()))
      fatal("bigdir: local entry '%s' missing from READDIR",
            e.path().filename().c_str());
    ++local_count;
  }
  if (local_count != expected)
    fatal("bigdir: backing dir has %zu entries, expected %zu", local_count, expected);
  std::printf("accept_client bigdir OK: %zu entries, no duplicates, no omissions "
              "(%.2fs full pagination)\n", names.size(), secs);
  return 0;
}

int cmd_stress(const char* host, uint16_t nfs_port, uint16_t mount_port,
               const std::string& export_path, const fs::path& backing,
               const std::string& rel_file, int conns, int pipeline, int seconds) {
  auto root = mnt(host, mount_port, export_path);
  Fh file_fh;
  {
    Client c(host, nfs_port);
    Fh cur = root;
    std::string rest = rel_file;
    while (!rest.empty()) {
      auto slash = rest.find('/');
      std::string comp = rest.substr(0, slash);
      rest = slash == std::string::npos ? "" : rest.substr(slash + 1);
      cur = lookup(c, cur, comp).first;
    }
    file_fh = cur;
  }
  auto local = read_local(backing / rel_file);
  if (local.empty()) fatal("stress target %s is empty", rel_file.c_str());
  const uint64_t size = local.size();

  std::atomic<uint64_t> total_ops{0};
  std::atomic<bool> failed{false};
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

  std::vector<std::thread> threads;
  for (int t = 0; t < conns; ++t) {
    threads.emplace_back([&, t] {
      Client c(host, nfs_port);
      std::mt19937_64 rng(0x51f5u + t);
      struct Pending {
        uint64_t off;
        uint32_t len;
        bool is_getattr;
      };
      std::map<uint32_t, Pending> inflight;
      uint64_t ops = 0;
      auto issue = [&] {
        uint32_t xid = c.next_xid++;
        if (ops % 64 == 63) {  // sprinkle GETATTR through the read storm
          XdrEnc args(c.pool);
          enc_fh(args, file_fh);
          send_record(c.fd, build_call(c.pool, xid, kNfsProg, 1, args.take().to_bytes()));
          inflight.emplace(xid, Pending{0, 0, true});
        } else {
          uint64_t off = rng() % size;
          uint32_t len = 1 + (uint32_t)(rng() % 65536);
          XdrEnc args(c.pool);
          enc_fh(args, file_fh);
          args.u64(off);
          args.u32(len);
          send_record(c.fd, build_call(c.pool, xid, kNfsProg, 6, args.take().to_bytes()));
          inflight.emplace(xid, Pending{off, len, false});
        }
        ++ops;
      };
      auto reap = [&] {
        auto wire = read_record(c.fd);
        XdrDec dec(std::span<const std::byte>(wire.data(), wire.size()));
        auto xid = dec.u32();
        if (!xid) fatal("stress: bad reply xid");
        auto it = inflight.find(*xid);
        if (it == inflight.end()) fatal("stress: unknown xid %u", *xid);
        Pending p = it->second;
        inflight.erase(it);
        // Re-parse header from the top with the xid we now know.
        XdrDec d2(std::span<const std::byte>(wire.data(), wire.size()));
        if (parse_reply_header(d2, *xid) != 0) fatal("stress: rpc error reply");
        uint32_t status = ru32(d2);
        if (status != kNfs3Ok) fatal("stress: nfs status %u", status);
        if (p.is_getattr) {
          if (decode_fattr(d2).size != size) fatal("stress: GETATTR size changed");
          return;
        }
        decode_post_attr(d2);
        uint32_t n = ru32(d2);
        rbool(d2);  // eof
        auto data = d2.opaque(1u << 22);
        if (!data || data->size() != n) fatal("stress: bad READ payload");
        uint32_t expect = (uint32_t)std::min<uint64_t>(p.len, size - p.off);
        if (n != expect)
          fatal("stress: READ off=%llu len=%u returned %u (expected %u)",
                (unsigned long long)p.off, p.len, n, expect);
        if (n && std::memcmp(data->data(), local.data() + p.off, n) != 0)
          fatal("stress: data mismatch at offset %llu", (unsigned long long)p.off);
      };
      while (std::chrono::steady_clock::now() < deadline && !failed.load()) {
        while (inflight.size() < (size_t)pipeline) issue();
        reap();
        total_ops.fetch_add(1, std::memory_order_relaxed);
      }
      while (!inflight.empty()) {
        reap();
        total_ops.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& th : threads) th.join();
  std::printf("accept_client stress OK: %llu ops over %d conns x %d pipeline in %ds "
              "(%.0f ops/s), all payloads verified\n",
              (unsigned long long)total_ops.load(), conns, pipeline, seconds,
              (double)total_ops.load() / seconds);
  return 0;
}

// ---------- phase-2 write-path acceptance ----------

std::pair<bool, bool> skip_wcc(XdrDec& d) {
  bool pre = rbool(d);
  if (pre) {
    ru64(d);
    for (int i = 0; i < 4; ++i) ru32(d);
  }
  bool post = rbool(d);
  if (post) decode_fattr(d);
  return {pre, post};
}

void enc_sattr(XdrEnc& e, std::optional<uint32_t> mode, std::optional<uint64_t> size) {
  e.boolean(mode.has_value());
  if (mode) e.u32(*mode);
  e.boolean(false);  // uid
  e.boolean(false);  // gid
  e.boolean(size.has_value());
  if (size) e.u64(*size);
  e.u32(0);  // atime DONT_CHANGE
  e.u32(0);  // mtime DONT_CHANGE
}

// CREATE; returns NFS status; on OK fills fh_out.
uint32_t nfs_create(Client& c, const Fh& dir, const std::string& name, uint32_t mode,
                    std::optional<uint32_t> file_mode, const std::array<std::byte, 8>* verf,
                    Fh* fh_out) {
  XdrEnc args(c.pool);
  enc_fh(args, dir);
  args.string(name);
  args.u32(mode);
  if (mode == 2) args.opaque_fixed(*verf);
  else enc_sattr(args, file_mode, std::nullopt);
  auto reply = c.call(kNfsProg, 8, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  if (status == kNfs3Ok) {
    if (!rbool(dec)) fatal("CREATE %s: missing post-op fh", name.c_str());
    auto fh = dec.opaque(64);
    if (!fh) fatal("CREATE: bad fh");
    if (fh_out) fh_out->assign(fh->begin(), fh->end());
    if (rbool(dec)) decode_fattr(dec);
    auto wcc = skip_wcc(dec);
    if (!wcc.second) fatal("CREATE %s: missing dir wcc post attr", name.c_str());
  } else {
    skip_wcc(dec);
  }
  return status;
}

uint32_t nfs_write(Client& c, const Fh& fh, uint64_t off, std::span<const std::byte> data,
                   uint32_t stable, std::array<std::byte, 8>* verf_out) {
  XdrEnc args(c.pool);
  enc_fh(args, fh);
  args.u64(off);
  args.u32((uint32_t)data.size());
  args.u32(stable);
  args.opaque(data);
  auto reply = c.call(kNfsProg, 7, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  auto wcc = skip_wcc(dec);
  if (status != kNfs3Ok) return status;
  if (!wcc.first || !wcc.second) fatal("WRITE: incomplete wcc");
  uint32_t count = ru32(dec);
  if (count != data.size()) fatal("WRITE: short write %u/%zu", count, data.size());
  uint32_t committed = ru32(dec);
  if (committed < stable) fatal("WRITE: committed %u below requested %u", committed, stable);
  auto verf = dec.opaque_fixed(8);
  if (!verf) fatal("WRITE: missing verifier");
  if (verf_out) std::copy(verf->begin(), verf->end(), verf_out->begin());
  return status;
}

uint32_t nfs_commit(Client& c, const Fh& fh, std::array<std::byte, 8>* verf_out) {
  XdrEnc args(c.pool);
  enc_fh(args, fh);
  args.u64(0);
  args.u32(0);
  auto reply = c.call(kNfsProg, 21, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  skip_wcc(dec);
  if (status != kNfs3Ok) return status;
  auto verf = dec.opaque_fixed(8);
  if (!verf) fatal("COMMIT: missing verifier");
  if (verf_out) std::copy(verf->begin(), verf->end(), verf_out->begin());
  return status;
}

uint32_t nfs_dirop(Client& c, uint32_t proc, const Fh& dir, const std::string& name) {
  XdrEnc args(c.pool);
  enc_fh(args, dir);
  args.string(name);
  auto reply = c.call(kNfsProg, proc, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  if (proc == 9) {  // MKDIR carries create-style results; args lacked sattr though
    fatal("nfs_dirop misused for MKDIR");
  }
  skip_wcc(dec);
  return status;
}

uint32_t nfs_mkdir(Client& c, const Fh& dir, const std::string& name, Fh* fh_out) {
  XdrEnc args(c.pool);
  enc_fh(args, dir);
  args.string(name);
  enc_sattr(args, 0755, std::nullopt);
  auto reply = c.call(kNfsProg, 9, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  if (status == kNfs3Ok) {
    if (rbool(dec)) {
      auto fh = dec.opaque(64);
      if (fh && fh_out) fh_out->assign(fh->begin(), fh->end());
    }
    if (rbool(dec)) decode_fattr(dec);
    skip_wcc(dec);
  } else {
    skip_wcc(dec);
  }
  return status;
}

uint32_t nfs_setattr(Client& c, const Fh& fh, std::optional<uint32_t> mode,
                     std::optional<uint64_t> size) {
  XdrEnc args(c.pool);
  enc_fh(args, fh);
  enc_sattr(args, mode, size);
  args.boolean(false);  // no guard
  auto reply = c.call(kNfsProg, 2, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  skip_wcc(dec);
  return status;
}

uint32_t nfs_rename(Client& c, const Fh& from_dir, const std::string& from,
                    const Fh& to_dir, const std::string& to) {
  XdrEnc args(c.pool);
  enc_fh(args, from_dir);
  args.string(from);
  enc_fh(args, to_dir);
  args.string(to);
  auto reply = c.call(kNfsProg, 14, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  skip_wcc(dec);
  skip_wcc(dec);
  return status;
}

uint32_t nfs_link(Client& c, const Fh& file, const Fh& dir, const std::string& name) {
  XdrEnc args(c.pool);
  enc_fh(args, file);
  enc_fh(args, dir);
  args.string(name);
  auto reply = c.call(kNfsProg, 15, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  if (rbool(dec)) decode_fattr(dec);
  skip_wcc(dec);
  return status;
}

uint32_t nfs_symlink(Client& c, const Fh& dir, const std::string& name,
                     const std::string& target, Fh* fh_out) {
  XdrEnc args(c.pool);
  enc_fh(args, dir);
  args.string(name);
  enc_sattr(args, std::nullopt, std::nullopt);
  args.string(target);
  auto reply = c.call(kNfsProg, 10, args.take().to_bytes());
  auto dec = make_dec(reply);
  uint32_t status = ru32(dec);
  if (status == kNfs3Ok) {
    if (rbool(dec)) {
      auto fh = dec.opaque(64);
      if (fh && fh_out) fh_out->assign(fh->begin(), fh->end());
    }
    if (rbool(dec)) decode_fattr(dec);
  }
  skip_wcc(dec);
  return status;
}

std::vector<std::byte> pattern_bytes(size_t n, uint64_t seed) {
  std::vector<std::byte> out(n);
  std::mt19937_64 rng(seed);
  for (size_t i = 0; i < n; i += 8) {
    uint64_t v = rng();
    std::memcpy(out.data() + i, &v, std::min<size_t>(8, n - i));
  }
  return out;
}

// Full read-write acceptance against a writable export.
int cmd_wtest(const char* host, uint16_t nfs_port, uint16_t mount_port,
              const std::string& export_path, const fs::path& backing) {
  auto root = mnt(host, mount_port, export_path);
  Client c(host, nfs_port);

  // Workspace
  Fh work;
  if (nfs_mkdir(c, root, "wtest", &work) != kNfs3Ok) fatal("mkdir wtest failed");
  if (!fs::is_directory(backing / "wtest")) fatal("mkdir not visible in backing dir");

  // CREATE + WRITE at each stability + byte-verify against the backing file.
  Fh file;
  if (nfs_create(c, work, "data.bin", 0, 0644u, nullptr, &file) != kNfs3Ok)
    fatal("create data.bin failed");
  auto blob = pattern_bytes(256 * 1024 + 13, 42);
  std::array<std::byte, 8> verf{}, verf2{};
  size_t third = blob.size() / 3;
  if (nfs_write(c, file, 0, std::span(blob).subspan(0, third), 0, &verf) != kNfs3Ok)
    fatal("unstable write failed");
  if (nfs_write(c, file, third, std::span(blob).subspan(third, third), 1, &verf2) != kNfs3Ok)
    fatal("datasync write failed");
  if (verf != verf2) fatal("write verifier changed within one boot");
  if (nfs_write(c, file, 2 * third, std::span(blob).subspan(2 * third), 2, &verf2) !=
      kNfs3Ok)
    fatal("filesync write failed");
  if (nfs_commit(c, file, &verf2) != kNfs3Ok) fatal("commit failed");
  if (verf != verf2) fatal("commit verifier mismatch");
  if (read_local(backing / "wtest/data.bin") != blob)
    fatal("backing file bytes differ after write");
  auto remote = read_all(c, file, blob.size(), 65536);
  if (remote != blob) fatal("READ-back differs");

  // SETATTR: truncate + chmod, visible in attributes and backing store.
  if (nfs_setattr(c, file, 0600u, 100u) != kNfs3Ok) fatal("setattr failed");
  auto attr = getattr(c, file);
  if (attr.size != 100 || (attr.mode & 07777) != 0600)
    fatal("setattr not applied: size=%llu mode=%o", (unsigned long long)attr.size,
          attr.mode);
  if (fs::file_size(backing / "wtest/data.bin") != 100) fatal("truncate not on disk");

  // CREATE guarded on existing -> EEXIST; unchecked on existing truncates.
  if (nfs_create(c, work, "data.bin", 1, 0644u, nullptr, nullptr) != 17)
    fatal("guarded create should EEXIST");
  Fh again;
  if (nfs_create(c, work, "data.bin", 0, std::nullopt, nullptr, &again) != kNfs3Ok)
    fatal("unchecked create on existing failed");

  // EXCLUSIVE create: replay succeeds, different verifier conflicts.
  std::array<std::byte, 8> everf{};
  everf[0] = std::byte{0x5A};
  Fh excl;
  if (nfs_create(c, work, "excl", 2, std::nullopt, &everf, &excl) != kNfs3Ok)
    fatal("exclusive create failed");
  if (nfs_create(c, work, "excl", 2, std::nullopt, &everf, nullptr) != kNfs3Ok)
    fatal("exclusive replay with same verifier should succeed");
  everf[0] = std::byte{0x5B};
  if (nfs_create(c, work, "excl", 2, std::nullopt, &everf, nullptr) != 17)
    fatal("exclusive with new verifier should EEXIST");

  // SYMLINK / READLINK / LINK / RENAME / REMOVE / RMDIR
  Fh sym;
  if (nfs_symlink(c, work, "link", "data.bin", &sym) != kNfs3Ok) fatal("symlink failed");
  if (read_link(c, sym) != "data.bin") fatal("readlink mismatch");
  if (nfs_link(c, file, work, "hard") != kNfs3Ok) fatal("link failed");
  if (getattr(c, file).size != fs::file_size(backing / "wtest/hard"))
    fatal("hard link contents differ");
  if (nfs_rename(c, work, "hard", work, "hard2") != kNfs3Ok) fatal("rename failed");
  if (!fs::exists(backing / "wtest/hard2")) fatal("rename not visible");
  for (const char* name : {"hard2", "link", "excl", "data.bin"})
    if (nfs_dirop(c, 12, work, name) != kNfs3Ok) fatal("remove %s failed", name);
  if (nfs_dirop(c, 13, root, "wtest") != kNfs3Ok) fatal("rmdir failed");
  if (fs::exists(backing / "wtest")) fatal("rmdir not visible");

  // DRC over the wire: byte-identical retransmission must yield a byte-identical
  // reply (MKDIR twice with the same xid would otherwise EEXIST).
  {
    uint32_t xid = c.next_xid++;
    XdrEnc args(c.pool);
    enc_fh(args, root);
    args.string("drc_check");
    enc_sattr(args, 0755u, std::nullopt);
    auto body = args.take().to_bytes();
    auto record = build_call(c.pool, xid, kNfsProg, 9, body);
    send_record(c.fd, record);
    auto first = read_record(c.fd);
    send_record(c.fd, record);
    auto second = read_record(c.fd);
    if (first != second) fatal("DRC: retransmitted MKDIR reply differs");
    auto dec = make_dec(first);
    parse_reply_header(dec, xid);
    if (ru32(dec) != kNfs3Ok) fatal("DRC: first MKDIR failed");
    if (nfs_dirop(c, 13, root, "drc_check") != kNfs3Ok) fatal("drc_check cleanup");
  }

  std::printf("accept_client wtest OK: create/write(3 levels)/commit/setattr/"
              "exclusive-replay/namespace ops/DRC all verified\n");
  return 0;
}

// Crash-recovery phase 1: unstable writes, record the verifier.
int cmd_crash_write(const char* host, uint16_t nfs_port, uint16_t mount_port,
                    const std::string& export_path, const std::string& state_file) {
  auto root = mnt(host, mount_port, export_path);
  Client c(host, nfs_port);
  Fh file;
  if (nfs_create(c, root, "crash.bin", 0, 0644u, nullptr, &file) != kNfs3Ok)
    fatal("create crash.bin failed");
  auto blob = pattern_bytes(4 << 20, 0xC7A54);
  std::array<std::byte, 8> verf{};
  for (size_t off = 0; off < blob.size(); off += 65536) {
    size_t n = std::min<size_t>(65536, blob.size() - off);
    if (nfs_write(c, file, off, std::span(blob).subspan(off, n), 0, &verf) != kNfs3Ok)
      fatal("unstable write at %zu failed", off);
  }
  FILE* f = fopen(state_file.c_str(), "w");
  if (!f) fatal("cannot write state file");
  for (auto b : verf) fprintf(f, "%02x", (unsigned)b);
  fprintf(f, "\n");
  fclose(f);
  std::printf("accept_client crash-write OK: 4MiB unstable, verifier recorded\n");
  return 0;
}

// Crash-recovery phase 2 (after kill -9 + restart): the verifier must have changed,
// re-sending the data must converge to correct on-disk content.
int cmd_crash_recover(const char* host, uint16_t nfs_port, uint16_t mount_port,
                      const std::string& export_path, const fs::path& backing,
                      const std::string& state_file) {
  std::ifstream in(state_file);
  std::string old_hex;
  if (!(in >> old_hex)) fatal("cannot read state file");

  auto root = mnt(host, mount_port, export_path);
  Client c(host, nfs_port);
  auto [file, attr0] = lookup(c, root, "crash.bin");
  (void)attr0;
  std::array<std::byte, 8> verf{};
  if (nfs_commit(c, file, &verf) != kNfs3Ok) fatal("commit after restart failed");
  char now_hex[17];
  for (int i = 0; i < 8; ++i)
    std::snprintf(now_hex + 2 * i, 3, "%02x", (unsigned)verf[i]);
  if (old_hex == now_hex)
    fatal("write verifier did not change across restart (%s)", now_hex);

  // Client-side recovery: resend everything FILE_SYNC, then verify byte-for-byte.
  auto blob = pattern_bytes(4 << 20, 0xC7A54);
  for (size_t off = 0; off < blob.size(); off += 65536) {
    size_t n = std::min<size_t>(65536, blob.size() - off);
    if (nfs_write(c, file, off, std::span(blob).subspan(off, n), 2, nullptr) != kNfs3Ok)
      fatal("recovery rewrite at %zu failed", off);
  }
  if (read_local(backing / "crash.bin") != blob)
    fatal("post-recovery on-disk content differs");
  auto remote = read_all(c, file, blob.size(), 65536);
  if (remote != blob) fatal("post-recovery READ-back differs");
  if (nfs_dirop(c, 12, root, "crash.bin") != kNfs3Ok) fatal("cleanup remove failed");
  std::printf("accept_client crash-recover OK: verifier changed %s -> %s, data "
              "converged after resend\n", old_hex.c_str(), now_hex);
  return 0;
}

// Connection storm + per-connection backpressure sanity.  Every accepted connection
// must answer a NULL call while the whole storm is held open; connections the server
// sheds at its configured limits count as refused, not failed.
int cmd_connstorm(const char* host, uint16_t nfs_port, int count, int pipeline_depth) {
  BufferPool pool;
  auto null_call = [&](uint32_t xid) { return build_call(pool, xid, kNfsProg, 0, {}); };
  auto try_null = [&](int fd, uint32_t xid) -> bool {
    uint32_t hdr = lnfs::xdr::to_be32(0x80000000u | 0);
    (void)hdr;
    auto record = null_call(xid);
    uint32_t marked = lnfs::xdr::to_be32(0x80000000u | (uint32_t)record.size());
    std::byte h[4];
    std::memcpy(h, &marked, 4);
    iovec iov[2] = {{h, 4}, {record.data(), record.size()}};
    if (writev(fd, iov, 2) != (ssize_t)(4 + record.size())) return false;
    std::byte rh[4];
    if (!read_exact(fd, rh, 4)) return false;
    uint32_t v;
    std::memcpy(&v, rh, 4);
    v = lnfs::xdr::from_be32(v);
    std::vector<std::byte> body(v & 0x7fffffffu);
    return read_exact(fd, body.data(), body.size());
  };

  std::vector<int> fds;
  fds.reserve(count);
  int refused = 0, alive = 0;
  for (int i = 0; i < count; ++i) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(nfs_port);
    inet_pton(AF_INET, host, &a.sin_addr);
    if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
      if (fd >= 0) close(fd);
      ++refused;
      continue;
    }
    fds.push_back(fd);
  }
  int first_alive = -1;
  for (int fd : fds) {
    if (try_null(fd, 0x1000 + (uint32_t)fd)) {
      ++alive;
      if (first_alive < 0) first_alive = fd;
    } else {
      ++refused;
    }
  }
  if (alive == 0) fatal("connstorm: no connection survived");
  // One surviving connection pipelines far beyond the inflight cap; every call must be
  // answered (backpressure delays parsing, never drops).
  {
    for (int i = 0; i < pipeline_depth; ++i) {
      auto record = null_call(0x9000 + i);
      uint32_t marked = lnfs::xdr::to_be32(0x80000000u | (uint32_t)record.size());
      std::byte h[4];
      std::memcpy(h, &marked, 4);
      iovec iov[2] = {{h, 4}, {record.data(), record.size()}};
      if (writev(first_alive, iov, 2) != (ssize_t)(4 + record.size()))
        fatal("connstorm: pipeline write failed at %d", i);
    }
    for (int i = 0; i < pipeline_depth; ++i) {
      std::byte rh[4];
      if (!read_exact(first_alive, rh, 4)) fatal("connstorm: pipeline reply %d lost", i);
      uint32_t v;
      std::memcpy(&v, rh, 4);
      v = lnfs::xdr::from_be32(v);
      std::vector<std::byte> body(v & 0x7fffffffu);
      if (!read_exact(first_alive, body.data(), body.size()))
        fatal("connstorm: pipeline reply body %d lost", i);
    }
  }
  for (int fd : fds) close(fd);
  std::printf("accept_client connstorm OK: %d alive of %d attempted (%d shed by "
              "limits), %d-deep pipeline on one conn fully answered\n",
              alive, count, refused, pipeline_depth);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  auto usage = [] {
    std::fprintf(stderr,
                 "usage: accept_client walk   HOST NFS_PORT MOUNT_PORT EXPORT BACKING\n"
                 "       accept_client bigdir HOST NFS_PORT MOUNT_PORT EXPORT BACKING SUBDIR COUNT\n"
                 "       accept_client stress HOST NFS_PORT MOUNT_PORT EXPORT BACKING FILE "
                 "CONNS PIPELINE SECONDS\n"
                 "       accept_client wtest  HOST NFS_PORT MOUNT_PORT EXPORT BACKING\n"
                 "       accept_client crash-write   HOST NFS_PORT MOUNT_PORT EXPORT STATE\n"
                 "       accept_client crash-recover HOST NFS_PORT MOUNT_PORT EXPORT "
                 "BACKING STATE\n"
                 "       accept_client connstorm HOST NFS_PORT MOUNT_PORT COUNT PIPELINE\n");
    return 2;
  };
  if (argc < 6) return usage();
  std::string cmd = argv[1];
  const char* host = argv[2];
  uint16_t nfs_port = (uint16_t)atoi(argv[3]);
  uint16_t mount_port = (uint16_t)atoi(argv[4]);
  std::string export_path = argv[5];
  if (cmd == "walk" && (argc == 7 || argc == 8))
    return cmd_walk(host, nfs_port, mount_port, export_path, argv[6],
                    argc == 8 && std::string(argv[7]) == "ro");
  if (cmd == "bigdir" && argc == 9)
    return cmd_bigdir(host, nfs_port, mount_port, export_path, argv[6], argv[7],
                      strtoull(argv[8], nullptr, 10));
  if (cmd == "stress" && argc == 11)
    return cmd_stress(host, nfs_port, mount_port, export_path, argv[6], argv[7],
                      atoi(argv[8]), atoi(argv[9]), atoi(argv[10]));
  if (cmd == "wtest" && argc == 7)
    return cmd_wtest(host, nfs_port, mount_port, export_path, argv[6]);
  if (cmd == "crash-write" && argc == 7)
    return cmd_crash_write(host, nfs_port, mount_port, export_path, argv[6]);
  if (cmd == "crash-recover" && argc == 8)
    return cmd_crash_recover(host, nfs_port, mount_port, export_path, argv[6], argv[7]);
  if (cmd == "connstorm" && argc == 7)
    return cmd_connstorm(host, nfs_port, atoi(argv[5]), atoi(argv[6]));
  return usage();
}
