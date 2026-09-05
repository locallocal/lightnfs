// Userspace NFSv3/MOUNTv3/NFSv4.1 acceptance client.
//
// Drives a *running* lightnfsd over real TCP and verifies the protocol surface
// against the export's backing directory — no kernel mount, no root. This is the
// loopback half of the acceptance suite (scripts/accept_m2_local.sh and
// accept_m6_local.sh); the kernel-client half lives in the accept_m*_vm.sh scripts.
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
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
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
#include <functional>
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
                                  uint32_t proc, const std::vector<std::byte>& body,
                                  uint32_t vers = kVers) {
  XdrEnc enc(pool);
  enc.u32(xid);
  enc.u32(0);  // CALL
  enc.u32(2);  // RPC v2
  enc.u32(prog);
  enc.u32(vers);
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
  if (!xid || *xid != expect_xid)
    fatal("reply xid mismatch: got %#x want %#x", xid ? *xid : 0u, expect_xid);
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
      case 3:   // BLK
      case 4:   // CHR
      case 6:   // SOCK
      case 7: {  // FIFO
        auto ftype = fs::symlink_status(child).type();
        bool ok = (attr.type == 3 && ftype == fs::file_type::block) ||
                  (attr.type == 4 && ftype == fs::file_type::character) ||
                  (attr.type == 6 && ftype == fs::file_type::socket) ||
                  (attr.type == 7 && ftype == fs::file_type::fifo);
        if (!ok) fatal("%s: special-file type mismatch", child.c_str());
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

// Fault injection (development plan §9 "故障注入"): the server runs with
// LNFS_FAULT_FSYNC_EIO=1.  The first fsync fails -> COMMIT answers NFS3ERR_IO, and the
// sticky-poison contract (design 06 §6.2) keeps every later COMMIT on that file failing
// even though the injected fault is spent; a different file syncs fine.
int cmd_fsync_eio(const char* host, uint16_t nfs_port, uint16_t mount_port,
                  const std::string& export_path) {
  auto root = mnt(host, mount_port, export_path);
  Client c(host, nfs_port);
  Fh work;
  if (nfs_mkdir(c, root, "fault", &work) != kNfs3Ok) fatal("fsync-eio: mkdir failed");
  Fh victim, healthy;
  if (nfs_create(c, work, "victim.bin", 0, 0644u, nullptr, &victim) != kNfs3Ok)
    fatal("fsync-eio: create victim failed");
  if (nfs_create(c, work, "healthy.bin", 0, 0644u, nullptr, &healthy) != kNfs3Ok)
    fatal("fsync-eio: create healthy failed");
  auto blob = pattern_bytes(64 * 1024, 7);
  if (nfs_write(c, victim, 0, blob, 0, nullptr) != kNfs3Ok) fatal("fsync-eio: UNSTABLE write");
  uint32_t st = nfs_commit(c, victim, nullptr);
  if (st != 5) fatal("fsync-eio: first COMMIT expected NFS3ERR_IO, got %u", st);
  for (int i = 0; i < 3; ++i) {
    st = nfs_commit(c, victim, nullptr);
    if (st != 5) fatal("fsync-eio: COMMIT #%d after poison expected IO, got %u", i + 2, st);
  }
  if (nfs_write(c, healthy, 0, blob, 2, nullptr) != kNfs3Ok)
    fatal("fsync-eio: FILE_SYNC write on a healthy file must succeed after the fault");
  if (nfs_commit(c, healthy, nullptr) != kNfs3Ok) fatal("fsync-eio: healthy COMMIT failed");
  std::printf("accept_client fsync-eio OK: injected EIO surfaced as NFS3ERR_IO, stayed "
              "sticky over 3 retries, unrelated file unaffected\n");
  return 0;
}

// ---------- NFSv4.1 acceptance (phase 3) ----------

namespace v4 {

[[maybe_unused]] constexpr uint32_t kOpAccess = 3, kOpGetattr = 9;
constexpr uint32_t kOpClose = 4, kOpGetfh = 10,
                   kOpLookup = 15, kOpOpen = 18, kOpPutfh = 22, kOpPutrootfh = 24,
                   kOpRead = 25, kOpReaddir = 26, kOpReadlink = 27,
                   kOpExchangeId = 42, kOpCreateSession = 43, kOpDestroySession = 44,
                   kOpSequence = 53, kOpDestroyClientid = 57, kOpReclaimComplete = 58;
constexpr uint32_t kAttrType = 1, kAttrSize = 4, kAttrFilehandle = 19, kAttrFileid = 20;

struct V4Client {
  Client rpc;
  uint64_t clientid = 0;
  std::array<std::byte, 16> sessionid{};
  uint32_t slot_seq = 1;
  uint32_t minor = 1;    // COMPOUND minorversion (2 for the v4.2 scenario)
  std::string owner_id;  // co_ownerid: distinct per simulated client
  std::string server_owner, server_scope;  // RFC 8881 §2.10.4 identity from EXCHANGE_ID

  explicit V4Client(const char* host, uint16_t port, std::string owner = "lightnfs-accept-v4")
      : rpc(host, port), owner_id(std::move(owner)) {}

  // Raw COMPOUND exchange; returns the reply payload past the RPC header.
  std::vector<std::byte> compound(const std::vector<std::byte>& body) {
    uint32_t xid = rpc.next_xid++;
    send_record(rpc.fd, build_call(rpc.pool, xid, kNfsProg, 1, body, 4));
    auto wire = read_record(rpc.fd);
    XdrDec dec(std::span<const std::byte>(wire.data(), wire.size()));
    if (parse_reply_header(dec, xid) != 0) fatal("v4: rpc-level error");
    return {wire.end() - dec.remaining(), wire.end()};
  }

  // Runs a compound, checks overall status, returns a decoder past {status,tag,count}.
  struct Res {
    std::vector<std::byte> bytes;
    uint32_t status = 0;
    XdrDec dec{std::span<const std::byte>{}};
  };
  Res run(const std::vector<std::byte>& body, uint32_t expect = 0,
          bool check = true) {
    Res out;
    out.bytes = compound(body);
    out.dec = XdrDec(std::span<const std::byte>(out.bytes.data(), out.bytes.size()));
    out.status = ru32(out.dec);
    (void)out.dec.opaque(1024);  // tag
    (void)ru32(out.dec);         // resarray count
    if (check && out.status != expect)
      fatal("v4 compound: status %u (expected %u)", out.status, expect);
    return out;
  }

  static void expect_op(XdrDec& d, uint32_t op, uint32_t status = 0) {
    uint32_t code = ru32(d);
    if (code != op) fatal("v4: resop %u where %u expected", code, op);
    uint32_t got = ru32(d);
    if (got != status) fatal("v4: op %u status %u (expected %u)", op, got, status);
  }
  static void skip_sequence_res(XdrDec& d) {
    expect_op(d, kOpSequence);
    (void)d.skip(16 + 5 * 4);
  }

  void establish(bool send_reclaim_complete = true) {
    BufferPool& pool = rpc.pool;
    XdrEnc ex(pool);
    ex.u32(0);
    ex.u32(minor);
    ex.u32(1);
    ex.u32(kOpExchangeId);
    std::array<std::byte, 8> verf{std::byte{0x42}};
    ex.opaque_fixed(verf);
    ex.string(owner_id);
    ex.u32(0);
    ex.u32(0);  // SP4_NONE
    ex.u32(0);  // no impl id
    auto r = run(ex.take().to_bytes());
    expect_op(r.dec, kOpExchangeId);
    clientid = ru64(r.dec);
    uint32_t seq = ru32(r.dec);
    (void)ru32(r.dec);  // flags
    (void)ru32(r.dec);  // state_protect: SP4_NONE
    (void)ru64(r.dec);  // server_owner.minor_id
    if (auto major = r.dec.opaque(1024))
      server_owner.assign(reinterpret_cast<const char*>(major->data()), major->size());
    if (auto scope = r.dec.opaque(1024))
      server_scope.assign(reinterpret_cast<const char*>(scope->data()), scope->size());

    XdrEnc cs(pool);
    cs.u32(0);
    cs.u32(minor);
    cs.u32(1);
    cs.u32(kOpCreateSession);
    cs.u64(clientid);
    cs.u32(seq);
    cs.u32(0);
    for (int chan = 0; chan < 2; ++chan) {
      cs.u32(0);          // headerpad
      cs.u32(1u << 20);   // maxreq
      cs.u32(1u << 20);   // maxresp
      cs.u32(8u << 10);   // maxresp cached
      cs.u32(16);         // maxops
      cs.u32(16);         // slots
      cs.u32(0);          // rdma_ird
    }
    cs.u32(0x40000000);
    cs.u32(1);
    cs.u32(0);  // AUTH_NONE cb cred
    auto c = run(cs.take().to_bytes());
    expect_op(c.dec, kOpCreateSession);
    auto sid = c.dec.opaque_fixed(16);
    if (!sid) fatal("v4: bad sessionid");
    std::copy(sid->begin(), sid->end(), sessionid.begin());

    if (!send_reclaim_complete) return;
    // RECLAIM_COMPLETE like a real client finishing recovery.
    XdrEnc rc(pool);
    seq_header(rc, 1);
    rc.u32(kOpReclaimComplete);
    rc.boolean(false);
    auto rr = run(rc.take().to_bytes(), 0, false);
    (void)rr;
  }

  void seq_header(XdrEnc& enc, uint32_t extra_ops, bool cachethis = false,
                  std::optional<uint32_t> force_seq = std::nullopt) {
    enc.u32(0);  // tag
    enc.u32(minor);
    enc.u32(1 + extra_ops);
    enc.u32(kOpSequence);
    enc.opaque_fixed(sessionid);
    enc.u32(force_seq.value_or(slot_seq));
    if (!force_seq) slot_seq++;
    enc.u32(0);  // slot 0
    enc.u32(0);  // highest
    enc.boolean(cachethis);
  }

  void destroy() {
    XdrEnc ds(rpc.pool);
    ds.u32(0);
    ds.u32(minor);
    ds.u32(1);
    ds.u32(kOpDestroySession);
    ds.opaque_fixed(sessionid);
    (void)run(ds.take().to_bytes());
    XdrEnc dc(rpc.pool);
    dc.u32(0);
    dc.u32(minor);
    dc.u32(1);
    dc.u32(kOpDestroyClientid);
    dc.u64(clientid);
    (void)run(dc.take().to_bytes());
  }
};

struct V4Entry {
  std::string name;
  uint32_t type = 0;
  uint64_t size = 0;
  std::vector<std::byte> fh;
};

// Parses a fattr4 that requested {type,size,filehandle,fileid}.
void parse_entry_attrs(XdrDec& d, V4Entry& out) {
  uint32_t words = ru32(d);
  uint32_t w0 = words > 0 ? ru32(d) : 0;
  for (uint32_t i = 1; i < words; ++i) (void)ru32(d);
  uint32_t vals_len = ru32(d);
  size_t before = d.remaining();
  if (w0 & (1u << kAttrType)) out.type = ru32(d);
  if (w0 & (1u << kAttrSize)) out.size = ru64(d);
  if (w0 & (1u << kAttrFilehandle)) {
    auto fh = d.opaque(128);
    if (!fh) fatal("v4: bad entry fh");
    out.fh.assign(fh->begin(), fh->end());
  }
  if (w0 & (1u << kAttrFileid)) (void)ru64(d);
  size_t consumed = before - d.remaining();
  if (consumed < vals_len) (void)d.skip(vals_len - consumed);
}

std::vector<std::byte> lookup_path(V4Client& c,
                                   const std::vector<std::string>& components) {
  XdrEnc ops(c.rpc.pool);
  c.seq_header(ops, 2 + (uint32_t)components.size());
  ops.u32(kOpPutrootfh);
  for (const auto& comp : components) {
    ops.u32(kOpLookup);
    ops.string(comp);
  }
  ops.u32(kOpGetfh);
  auto r = c.run(ops.take().to_bytes());
  V4Client::skip_sequence_res(r.dec);
  V4Client::expect_op(r.dec, kOpPutrootfh);
  for (size_t i = 0; i < components.size(); ++i) V4Client::expect_op(r.dec, kOpLookup);
  V4Client::expect_op(r.dec, kOpGetfh);
  auto fh = r.dec.opaque(128);
  if (!fh) fatal("v4: bad path fh");
  return {fh->begin(), fh->end()};
}

std::vector<std::byte> v4_read_all(V4Client& c, const std::vector<std::byte>& dir_fh,
                                   const std::string& name, uint64_t size) {
  // OPEN(CLAIM_NULL) -> READ loop -> CLOSE, like a kernel client.
  XdrEnc open_ops(c.rpc.pool);
  c.seq_header(open_ops, 3);
  open_ops.u32(kOpPutfh);
  open_ops.opaque(dir_fh);
  open_ops.u32(kOpOpen);
  open_ops.u32(0);
  open_ops.u32(1);  // READ
  open_ops.u32(0);
  open_ops.u64(c.clientid);
  open_ops.string("accept-owner");
  open_ops.u32(0);  // NOCREATE
  open_ops.u32(0);  // CLAIM_NULL
  open_ops.string(name);
  open_ops.u32(kOpGetfh);
  auto r = c.run(open_ops.take().to_bytes());
  V4Client::skip_sequence_res(r.dec);
  V4Client::expect_op(r.dec, kOpPutfh);
  V4Client::expect_op(r.dec, kOpOpen);
  uint32_t sid_seq = ru32(r.dec);
  auto sid_other = r.dec.opaque_fixed(12);
  if (!sid_other) fatal("v4: bad open stateid");
  std::array<std::byte, 12> other{};
  std::copy(sid_other->begin(), sid_other->end(), other.begin());
  rbool(r.dec);
  ru64(r.dec);
  ru64(r.dec);
  ru32(r.dec);  // rflags
  uint32_t maskw = ru32(r.dec);
  for (uint32_t i = 0; i < maskw; ++i) (void)ru32(r.dec);
  ru32(r.dec);  // delegation none
  V4Client::expect_op(r.dec, kOpGetfh);
  auto file_fh_span = r.dec.opaque(128);
  std::vector<std::byte> file_fh(file_fh_span->begin(), file_fh_span->end());

  std::vector<std::byte> data;
  bool eof = size == 0;
  while (!eof) {
    XdrEnc rd(c.rpc.pool);
    c.seq_header(rd, 2);
    rd.u32(kOpPutfh);
    rd.opaque(file_fh);
    rd.u32(kOpRead);
    rd.u32(sid_seq);
    rd.opaque_fixed(other);
    rd.u64(data.size());
    rd.u32(65536);
    auto rr = c.run(rd.take().to_bytes());
    V4Client::skip_sequence_res(rr.dec);
    V4Client::expect_op(rr.dec, kOpPutfh);
    V4Client::expect_op(rr.dec, kOpRead);
    eof = rbool(rr.dec);
    auto chunk = rr.dec.opaque(1u << 20);
    if (!chunk) fatal("v4: bad read data");
    data.insert(data.end(), chunk->begin(), chunk->end());
    if (chunk->empty() && !eof) fatal("v4: empty read without eof");
    if (data.size() > size) fatal("v4: read beyond expected size");
  }

  XdrEnc cl(c.rpc.pool);
  c.seq_header(cl, 2);
  cl.u32(kOpPutfh);
  cl.opaque(file_fh);
  cl.u32(kOpClose);
  cl.u32(0);
  cl.u32(sid_seq);
  cl.opaque_fixed(other);
  auto cr = c.run(cl.take().to_bytes());
  (void)cr;
  return data;
}

void v4_walk_dir(V4Client& c, const std::vector<std::byte>& dir_fh,
                 const fs::path& local, size_t& dirs, size_t& files, size_t& bytes) {
  ++dirs;
  std::unordered_set<std::string> local_names;
  for (const auto& e : fs::directory_iterator(local))
    local_names.insert(e.path().filename().string());

  std::vector<V4Entry> entries;
  uint64_t cookie = 0;
  bool eof = false;
  while (!eof) {
    XdrEnc ops(c.rpc.pool);
    c.seq_header(ops, 2);
    ops.u32(kOpPutfh);
    ops.opaque(dir_fh);
    ops.u32(kOpReaddir);
    ops.u64(cookie);
    std::array<std::byte, 8> verf{};
    ops.opaque_fixed(verf);
    ops.u32(1u << 16);
    ops.u32(1u << 17);
    ops.u32(1);  // bitmap: one word
    ops.u32((1u << kAttrType) | (1u << kAttrSize) | (1u << kAttrFilehandle) |
            (1u << kAttrFileid));
    auto r = c.run(ops.take().to_bytes());
    V4Client::skip_sequence_res(r.dec);
    V4Client::expect_op(r.dec, kOpPutfh);
    V4Client::expect_op(r.dec, kOpReaddir);
    (void)r.dec.opaque_fixed(8);
    size_t page = 0;
    while (rbool(r.dec)) {
      V4Entry ent;
      cookie = ru64(r.dec);
      auto name = r.dec.string(255);
      if (!name) fatal("v4: bad readdir name");
      ent.name = std::string(*name);
      parse_entry_attrs(r.dec, ent);
      entries.push_back(std::move(ent));
      ++page;
    }
    eof = rbool(r.dec);
    if (!eof && page == 0) fatal("v4: empty readdir page without eof");
  }

  if (entries.size() != local_names.size())
    fatal("%s: v4 lists %zu entries, backing has %zu", local.c_str(), entries.size(),
          local_names.size());
  for (auto& ent : entries) {
    if (!local_names.count(ent.name))
      fatal("%s: v4 entry '%s' missing locally", local.c_str(), ent.name.c_str());
    fs::path child = local / ent.name;
    if (ent.type == 2) {  // DIR
      if (ent.fh.empty()) fatal("v4: dir entry without fh");
      v4_walk_dir(c, ent.fh, child, dirs, files, bytes);
    } else if (ent.type == 1) {  // REG
      if (fs::file_size(child) != ent.size)
        fatal("%s: v4 size mismatch", child.c_str());
      if (ent.size > 0 && ent.size <= (4u << 20)) {
        auto remote = v4_read_all(c, dir_fh, ent.name, ent.size);
        if (remote != read_local(child)) fatal("%s: v4 content mismatch", child.c_str());
        bytes += remote.size();
      }
      ++files;
    } else if (ent.type == 5) {  // LNK
      XdrEnc rl(c.rpc.pool);
      c.seq_header(rl, 2);
      rl.u32(kOpPutfh);
      rl.opaque(ent.fh);
      rl.u32(kOpReadlink);
      auto rr = c.run(rl.take().to_bytes());
      V4Client::skip_sequence_res(rr.dec);
      V4Client::expect_op(rr.dec, kOpPutfh);
      V4Client::expect_op(rr.dec, kOpReadlink);
      auto target = rr.dec.string(1024);
      if (!target || *target != fs::read_symlink(child).string())
        fatal("%s: v4 symlink mismatch", child.c_str());
    }
  }
}

// ---------- NFSv4.1 read-write acceptance (phase 4) ----------

constexpr uint32_t kOpCommit = 5, kOpCreate = 6, kOpLock = 12, kOpLockt = 13,
                   kOpLocku = 14, kOpLink = 11, kOpOpenDowngrade = 21,
                   kOpRemove = 28, kOpRename = 29, kOpSavefh = 32, kOpSetattr = 34,
                   kOpWrite = 38;
constexpr uint32_t kShareDenied = 10015, kOpenmode = 10038, kGrace = 10013,
                   kStaleStateid = 10023, kNoGrace = 10033;

struct Stateid4 {
  uint32_t seqid = 0;
  std::array<std::byte, 12> other{};
  void encode(XdrEnc& e) const {
    e.u32(seqid);
    e.opaque_fixed(other);
  }
  static Stateid4 decode(XdrDec& d) {
    Stateid4 s;
    s.seqid = ru32(d);
    auto o = d.opaque_fixed(12);
    if (!o) fatal("v4: bad stateid");
    std::copy(o->begin(), o->end(), s.other.begin());
    return s;
  }
};

struct OpenOut {
  uint32_t status = 0;
  Stateid4 stateid;
  std::vector<std::byte> fh;
  bool other_differs(const OpenOut& o) const { return stateid.other != o.stateid.other; }
};

// {SEQUENCE, PUTFH dir|file, OPEN, GETFH}: claim 0 (name) or 4 (CLAIM_FH) or 1
// (CLAIM_PREVIOUS); create=true → OPEN4_CREATE UNCHECKED with mode 0644.
OpenOut v4_open(V4Client& c, const std::vector<std::byte>& fh, const std::string& name,
                uint32_t access, uint32_t deny, const std::string& owner, bool create,
                uint32_t claim = 0, std::optional<uint64_t> size = std::nullopt) {
  XdrEnc ops(c.rpc.pool);
  c.seq_header(ops, 3);
  ops.u32(kOpPutfh);
  ops.opaque(fh);
  ops.u32(kOpOpen);
  ops.u32(0);
  ops.u32(access);
  ops.u32(deny);
  ops.u64(c.clientid);
  ops.string(owner);
  if (create) {
    ops.u32(1);  // OPEN4_CREATE
    ops.u32(0);  // UNCHECKED4
    ops.u32(2);  // bitmap: 2 words
    ops.u32(size ? (1u << kAttrSize) : 0);
    ops.u32(1u << (33 - 32));  // mode
    XdrEnc vals(c.rpc.pool);
    if (size) vals.u64(*size);
    vals.u32(0644);
    auto bytes = vals.take().to_bytes();
    ops.opaque(bytes);
  } else {
    ops.u32(0);
  }
  ops.u32(claim);
  if (claim == 0) ops.string(name);
  else if (claim == 1) ops.u32(0);
  ops.u32(kOpGetfh);
  auto r = c.run(ops.take().to_bytes(), 0, false);
  OpenOut out;
  out.status = r.status;
  if (r.status != 0) return out;
  V4Client::skip_sequence_res(r.dec);
  V4Client::expect_op(r.dec, kOpPutfh);
  V4Client::expect_op(r.dec, kOpOpen);
  out.stateid = Stateid4::decode(r.dec);
  rbool(r.dec);
  ru64(r.dec);
  ru64(r.dec);
  ru32(r.dec);
  uint32_t words = ru32(r.dec);
  for (uint32_t i = 0; i < words; ++i) (void)ru32(r.dec);
  ru32(r.dec);
  V4Client::expect_op(r.dec, kOpGetfh);
  auto span = r.dec.opaque(128);
  if (!span) fatal("v4: bad open fh");
  out.fh.assign(span->begin(), span->end());
  return out;
}

uint32_t v4_write(V4Client& c, const std::vector<std::byte>& fh, const Stateid4& sid,
                  uint64_t offset, std::span<const std::byte> data, uint32_t stable,
                  std::array<std::byte, 8>* verf = nullptr) {
  XdrEnc ops(c.rpc.pool);
  c.seq_header(ops, 2);
  ops.u32(kOpPutfh);
  ops.opaque(fh);
  ops.u32(kOpWrite);
  sid.encode(ops);
  ops.u64(offset);
  ops.u32(stable);
  ops.opaque(data);
  auto r = c.run(ops.take().to_bytes(), 0, false);
  if (r.status != 0) return r.status;
  V4Client::skip_sequence_res(r.dec);
  V4Client::expect_op(r.dec, kOpPutfh);
  V4Client::expect_op(r.dec, kOpWrite);
  uint32_t n = ru32(r.dec);
  if (n != data.size()) fatal("v4: short write %u of %zu", n, data.size());
  (void)ru32(r.dec);
  auto v = r.dec.opaque_fixed(8);
  if (!v) fatal("v4: bad write verifier");
  if (verf) std::copy(v->begin(), v->end(), verf->begin());
  return 0;
}

std::array<std::byte, 8> v4_commit(V4Client& c, const std::vector<std::byte>& fh) {
  XdrEnc ops(c.rpc.pool);
  c.seq_header(ops, 2);
  ops.u32(kOpPutfh);
  ops.opaque(fh);
  ops.u32(kOpCommit);
  ops.u64(0);
  ops.u32(0);
  auto r = c.run(ops.take().to_bytes());
  V4Client::skip_sequence_res(r.dec);
  V4Client::expect_op(r.dec, kOpPutfh);
  V4Client::expect_op(r.dec, kOpCommit);
  auto v = r.dec.opaque_fixed(8);
  std::array<std::byte, 8> out{};
  std::copy(v->begin(), v->end(), out.begin());
  return out;
}

std::vector<std::byte> v4_read(V4Client& c, const std::vector<std::byte>& fh,
                               const Stateid4& sid, uint64_t size, uint32_t* status = nullptr) {
  std::vector<std::byte> data;
  bool eof = size == 0;
  while (!eof) {
    XdrEnc rd(c.rpc.pool);
    c.seq_header(rd, 2);
    rd.u32(kOpPutfh);
    rd.opaque(fh);
    rd.u32(kOpRead);
    sid.encode(rd);
    rd.u64(data.size());
    rd.u32(65536);
    auto rr = c.run(rd.take().to_bytes(), 0, false);
    if (rr.status != 0) {
      if (status) *status = rr.status;
      return data;
    }
    V4Client::skip_sequence_res(rr.dec);
    V4Client::expect_op(rr.dec, kOpPutfh);
    V4Client::expect_op(rr.dec, kOpRead);
    eof = rbool(rr.dec);
    auto chunk = rr.dec.opaque(1u << 20);
    if (!chunk) fatal("v4: bad read data");
    data.insert(data.end(), chunk->begin(), chunk->end());
    if (chunk->empty() && !eof) fatal("v4: empty read without eof");
    if (data.size() >= size) break;
  }
  if (status) *status = 0;
  return data;
}

uint32_t v4_close(V4Client& c, const std::vector<std::byte>& fh, const Stateid4& sid) {
  XdrEnc cl(c.rpc.pool);
  c.seq_header(cl, 2);
  cl.u32(kOpPutfh);
  cl.opaque(fh);
  cl.u32(kOpClose);
  cl.u32(0);
  sid.encode(cl);
  return c.run(cl.take().to_bytes(), 0, false).status;
}

uint32_t v4_setattr_size(V4Client& c, const std::vector<std::byte>& fh, const Stateid4& sid,
                         uint64_t size) {
  XdrEnc ops(c.rpc.pool);
  c.seq_header(ops, 2);
  ops.u32(kOpPutfh);
  ops.opaque(fh);
  ops.u32(kOpSetattr);
  sid.encode(ops);
  ops.u32(1);
  ops.u32(1u << kAttrSize);
  XdrEnc vals(c.rpc.pool);
  vals.u64(size);
  auto bytes = vals.take().to_bytes();
  ops.opaque(bytes);
  return c.run(ops.take().to_bytes(), 0, false).status;
}

uint32_t v4_dir_op(V4Client& c, const std::vector<std::byte>& dir_fh,
                   const std::function<void(XdrEnc&)>& body, uint32_t extra = 0,
                   const std::function<void(XdrEnc&)>& prefix = {}) {
  XdrEnc ops(c.rpc.pool);
  c.seq_header(ops, 2 + extra);
  if (prefix) prefix(ops);
  ops.u32(kOpPutfh);
  ops.opaque(dir_fh);
  body(ops);
  return c.run(ops.take().to_bytes(), 0, false).status;
}

uint32_t v4_reclaim_complete(V4Client& c) {
  XdrEnc rc(c.rpc.pool);
  c.seq_header(rc, 1);
  rc.u32(kOpReclaimComplete);
  rc.boolean(false);
  return c.run(rc.take().to_bytes(), 0, false).status;
}

std::vector<std::string> split_path(const std::string& path) {
  std::vector<std::string> components;
  size_t pos = 0;
  while (pos < path.size()) {
    while (pos < path.size() && path[pos] == '/') ++pos;
    size_t end = path.find('/', pos);
    if (pos < path.size())
      components.push_back(path.substr(pos, end == std::string::npos ? std::string::npos
                                                                     : end - pos));
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return components;
}

std::vector<std::byte> random_bytes(size_t n, uint32_t seed) {
  std::vector<std::byte> out(n);
  uint32_t x = seed ? seed : 1;
  for (auto& b : out) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    b = static_cast<std::byte>(x);
  }
  return out;
}

}  // namespace v4

// Full v4.1 read-write acceptance (development plan §6.3 loopback half): create,
// chunked UNSTABLE writes + COMMIT verifier, byte-verified read-back and backing-file
// comparison, SETATTR truncate, share reservation across two clients, OPEN_DOWNGRADE,
// namespace ops (CREATE dir / RENAME / LINK / REMOVE) mirrored on the backing tree.
int cmd_v4rw(const char* host, uint16_t nfs_port, const std::string& export_path,
             const fs::path& backing) {
  using namespace v4;
  V4Client a(host, nfs_port);
  a.establish();
  auto root = lookup_path(a, split_path(export_path));

  // Create + write 3 MiB in 64 KiB UNSTABLE chunks, then COMMIT.
  auto payload = random_bytes(3u << 20, 0x1234);
  auto o = v4_open(a, root, "v4rw.bin", 3, 0, "rw-owner", true);
  if (o.status != 0) fatal("v4rw: OPEN(CREATE) status %u", o.status);
  std::array<std::byte, 8> wverf{}, cverf{};
  for (size_t off = 0; off < payload.size(); off += 65536) {
    size_t len = std::min<size_t>(65536, payload.size() - off);
    uint32_t st = v4_write(a, o.fh, o.stateid, off,
                           std::span<const std::byte>(payload.data() + off, len), 0, &wverf);
    if (st != 0) fatal("v4rw: WRITE status %u at %zu", st, off);
  }
  cverf = v4_commit(a, o.fh);
  if (cverf != wverf) fatal("v4rw: COMMIT verifier differs from WRITE verifier");
  if (read_local(backing / "v4rw.bin") != payload) fatal("v4rw: backing file mismatch");
  if (v4_read(a, o.fh, o.stateid, payload.size()) != payload)
    fatal("v4rw: read-back mismatch");

  // Truncate via SETATTR(size) with the open stateid.
  if (uint32_t st = v4_setattr_size(a, o.fh, o.stateid, 1u << 20); st != 0)
    fatal("v4rw: SETATTR size status %u", st);
  if (fs::file_size(backing / "v4rw.bin") != (1u << 20)) fatal("v4rw: truncate not applied");

  // Share reservation across clients: a re-opens with deny WRITE (same owner merges,
  // seqid++); b's WRITE open is SHARE_DENIED; anonymous WRITE is LOCKED; after
  // OPEN_DOWNGRADE drops the deny, b succeeds.
  auto o2 = v4_open(a, root, "v4rw.bin", 3, 2, "rw-owner", false);
  if (o2.status != 0 || o2.other_differs(o)) fatal("v4rw: merge OPEN status %u", o2.status);
  if (o2.stateid.seqid != o.stateid.seqid + 1) fatal("v4rw: merged seqid not bumped");
  V4Client b(host, nfs_port, "lightnfs-accept-v4-b");
  b.establish();
  auto ob = v4_open(b, root, "v4rw.bin", 2, 0, "b-owner", false);
  if (ob.status != kShareDenied) fatal("v4rw: expected SHARE_DENIED, got %u", ob.status);
  {
    Stateid4 anon{};
    std::array<std::byte, 4> x{};
    uint32_t st = v4_write(b, o.fh, anon, 0, x, 0);
    if (st != 10012) fatal("v4rw: anonymous write vs deny: expected LOCKED, got %u", st);
    auto ro = v4_open(b, root, "v4rw.bin", 1, 0, "b-owner", false);
    if (ro.status != 0) fatal("v4rw: read OPEN under deny WRITE status %u", ro.status);
    st = v4_write(b, ro.fh, ro.stateid, 0, x, 0);
    if (st != kOpenmode) fatal("v4rw: write via read-only open: expected OPENMODE, got %u", st);
    if (v4_close(b, ro.fh, ro.stateid) != 0) fatal("v4rw: CLOSE b/ro failed");
  }
  {
    XdrEnc dg(a.rpc.pool);
    a.seq_header(dg, 2);
    dg.u32(kOpPutfh);
    dg.opaque(o.fh);
    dg.u32(kOpOpenDowngrade);
    o2.stateid.encode(dg);
    dg.u32(0);
    dg.u32(3);
    dg.u32(0);
    auto r = a.run(dg.take().to_bytes());
    V4Client::skip_sequence_res(r.dec);
    V4Client::expect_op(r.dec, kOpPutfh);
    V4Client::expect_op(r.dec, kOpOpenDowngrade);
    o2.stateid = Stateid4::decode(r.dec);
  }
  ob = v4_open(b, root, "v4rw.bin", 2, 0, "b-owner", false);
  if (ob.status != 0) fatal("v4rw: OPEN after downgrade status %u", ob.status);
  if (v4_close(b, ob.fh, ob.stateid) != 0) fatal("v4rw: CLOSE b failed");
  // Stale seqid CLOSE is OLD_STATEID; the current one succeeds.
  if (v4_close(a, o.fh, o.stateid) != 10024) fatal("v4rw: expected OLD_STATEID on stale CLOSE");
  if (v4_close(a, o.fh, o2.stateid) != 0) fatal("v4rw: CLOSE a failed");

  // Namespace: CREATE dir, RENAME file into it, LINK, REMOVE — mirrored on backing.
  uint32_t st = v4_dir_op(a, root, [&](XdrEnc& e) {
    e.u32(kOpCreate);
    e.u32(2);  // NF4DIR
    e.string("v4dir");
    e.u32(0);  // empty bitmap
    e.u32(0);  // empty attrlist
  });
  if (st != 0) fatal("v4rw: CREATE dir status %u", st);
  if (!fs::is_directory(backing / "v4dir")) fatal("v4rw: v4dir missing on backing");
  auto dir_fh = lookup_path(a, [&] {
    auto c = split_path(export_path);
    c.push_back("v4dir");
    return c;
  }());
  st = v4_dir_op(a, dir_fh, [&](XdrEnc& e) {
    e.u32(kOpRename);
    e.string("v4rw.bin");
    e.string("moved.bin");
  }, 2, [&](XdrEnc& e) {
    e.u32(kOpPutfh);
    e.opaque(root);
    e.u32(kOpSavefh);
  });
  if (st != 0) fatal("v4rw: RENAME status %u", st);
  if (!fs::exists(backing / "v4dir" / "moved.bin") || fs::exists(backing / "v4rw.bin"))
    fatal("v4rw: RENAME not reflected on backing");
  auto moved_fh = lookup_path(a, [&] {
    auto c = split_path(export_path);
    c.push_back("v4dir");
    c.push_back("moved.bin");
    return c;
  }());
  st = v4_dir_op(a, root, [&](XdrEnc& e) {
    e.u32(kOpLink);
    e.string("hardlink.bin");
  }, 2, [&](XdrEnc& e) {
    e.u32(kOpPutfh);
    e.opaque(moved_fh);
    e.u32(kOpSavefh);
  });
  if (st != 0) fatal("v4rw: LINK status %u", st);
  if (fs::hard_link_count(backing / "hardlink.bin") != 2) fatal("v4rw: LINK not reflected");
  st = v4_dir_op(a, root, [&](XdrEnc& e) {
    e.u32(kOpRemove);
    e.string("v4dir");
  });
  if (st != 66) fatal("v4rw: REMOVE non-empty dir: expected NOTEMPTY, got %u", st);
  for (auto [fh, name] : {std::pair{dir_fh, "moved.bin"}, std::pair{root, "hardlink.bin"},
                          std::pair{root, "v4dir"}}) {
    st = v4_dir_op(a, fh, [&](XdrEnc& e) {
      e.u32(kOpRemove);
      e.string(name);
    });
    if (st != 0) fatal("v4rw: REMOVE %s status %u", name, st);
  }
  if (fs::exists(backing / "v4dir") || fs::exists(backing / "hardlink.bin"))
    fatal("v4rw: REMOVE not reflected on backing");

  b.destroy();
  a.destroy();
  std::printf("accept_client v4rw OK: %zu bytes written/committed/read back, truncate, "
              "share deny across clients, OPENMODE/LOCKED/OLD_STATEID discipline, "
              "downgrade, dir/rename/link/remove mirrored on backing\n", payload.size());
  return 0;
}

namespace v4 { struct LockDenied { uint64_t offset=0, length=0; uint64_t clientid=0; std::string owner; }; }

// v4.1 byte-range lock acceptance (development plan §6.3/§7): two clients contend over
// a file — LOCK (new + existing owner), LOCKT probe, DENIED with holder info, upgrade,
// LOCKU release and IO gated by lock/open mode; every effect cross-checked.
int cmd_v4lock(const char* host, uint16_t nfs_port, const std::string& export_path,
               const fs::path& backing) {
  using namespace v4;
  V4Client a(host, nfs_port);
  a.establish();
  auto root = lookup_path(a, split_path(export_path));
  auto payload = random_bytes(4096, 0x51);
  auto oa = v4_open(a, root, "v4lock.bin", 3, 0, "lock-owner-a", true);
  if (oa.status != 0) fatal("v4lock: OPEN(CREATE) status %u", oa.status);
  if (v4_write(a, oa.fh, oa.stateid, 0, payload, 2) != 0) fatal("v4lock: seed WRITE failed");

  auto do_lock = [&](v4::V4Client& c, const std::vector<std::byte>& fh, uint32_t type,
                     uint64_t off, uint64_t len, bool new_owner, const v4::Stateid4& sid,
                     const std::string& owner, v4::Stateid4* out, v4::LockDenied* denied) {
    XdrEnc ops(c.rpc.pool);
    c.seq_header(ops, 2);
    ops.u32(kOpPutfh);
    ops.opaque(fh);
    ops.u32(kOpLock);
    ops.u32(type);
    ops.boolean(false);
    ops.u64(off);
    ops.u64(len);
    ops.boolean(new_owner);
    if (new_owner) {
      ops.u32(0);
      sid.encode(ops);
      ops.u32(0);
      ops.u64(c.clientid);
      ops.string(owner);
    } else {
      sid.encode(ops);
      ops.u32(0);
    }
    auto r = c.run(ops.take().to_bytes(), 0, false);
    V4Client::skip_sequence_res(r.dec);
    V4Client::expect_op(r.dec, kOpPutfh);
    uint32_t opcode = ru32(r.dec);
    uint32_t code = ru32(r.dec);
    if (opcode != kOpLock) fatal("v4lock: expected LOCK resop, got op=%u status=%u", opcode, code);
    if (code == 0 && out) *out = Stateid4::decode(r.dec);
    else if (code == 10010 && denied) {  // NFS4ERR_DENIED
      denied->offset = ru64(r.dec);
      denied->length = ru64(r.dec);
      ru32(r.dec);
      denied->clientid = ru64(r.dec);
      auto o = r.dec.opaque(1024);
      denied->owner.assign(reinterpret_cast<const char*>(o->data()), o->size());
    }
    return code;
  };

  // a: exclusive [0,100).
  v4::Stateid4 lsid;
  if (do_lock(a, oa.fh, 2, 0, 100, true, oa.stateid, "lo-a", &lsid, nullptr) != 0)
    fatal("v4lock: a WRITE_LT failed");
  if (lsid.seqid != 1 || (lsid.other[4] != std::byte{2}))
    fatal("v4lock: lock stateid malformed");

  // b: overlapping read lock -> DENIED naming a; disjoint -> OK.
  V4Client b(host, nfs_port, "lightnfs-accept-v4-b");
  b.establish();
  auto ob = v4_open(b, root, "v4lock.bin", 3, 0, "lock-owner-b", false);
  if (ob.status != 0) fatal("v4lock: b OPEN status %u", ob.status);
  v4::LockDenied denied;
  if (do_lock(b, ob.fh, 1, 50, 10, true, ob.stateid, "lo-b", nullptr, &denied) != 10010)
    fatal("v4lock: expected DENIED for b's overlapping lock");
  if (denied.clientid != a.clientid || denied.owner != "lo-a" || denied.offset != 0 ||
      denied.length != 100)
    fatal("v4lock: DENIED holder info wrong (client=%llx owner=%s [%llu,%llu))",
          (unsigned long long)denied.clientid, denied.owner.c_str(),
          (unsigned long long)denied.offset, (unsigned long long)denied.length);
  v4::Stateid4 lsb;
  if (do_lock(b, ob.fh, 2, 100, 100, true, ob.stateid, "lo-b", &lsb, nullptr) != 0)
    fatal("v4lock: b disjoint lock failed");

  // LOCKT: b probing a's range -> DENIED; a probing its own -> OK.
  auto do_lockt = [&](v4::V4Client& c, const std::string& owner, uint64_t off, uint64_t len) {
    XdrEnc ops(c.rpc.pool);
    c.seq_header(ops, 2);
    ops.u32(kOpPutfh);
    ops.opaque(oa.fh);
    ops.u32(kOpLockt);
    ops.u32(2);
    ops.u64(off);
    ops.u64(len);
    ops.u64(c.clientid);
    ops.string(owner);
    auto r = c.run(ops.take().to_bytes(), 0, false);
    V4Client::skip_sequence_res(r.dec);
    V4Client::expect_op(r.dec, kOpPutfh);
    ru32(r.dec);  // opcode
    return ru32(r.dec);
  };
  if (do_lockt(b, "lo-b", 10, 5) != 10010) fatal("v4lock: LOCKT should be DENIED");
  if (do_lockt(a, "lo-a", 10, 5) != 0) fatal("v4lock: own-range LOCKT should be OK");

  // a upgrades the existing lock-owner over [200,50) via the lock stateid, then LOCKU
  // the whole file; b can then lock what a held.
  v4::Stateid4 lsid2;
  if (do_lock(a, oa.fh, 2, 200, 50, false, lsid, "", &lsid2, nullptr) != 0)
    fatal("v4lock: a existing-owner extend failed");
  if (!(lsid2.other == lsid.other) || lsid2.seqid != 2)
    fatal("v4lock: existing-owner lock did not bump seqid on same stateid");
  auto do_locku = [&](v4::V4Client& c, const v4::Stateid4& sid, uint64_t off, uint64_t len) {
    XdrEnc ops(c.rpc.pool);
    c.seq_header(ops, 2);
    ops.u32(kOpPutfh);
    ops.opaque(oa.fh);
    ops.u32(kOpLocku);
    ops.u32(2);
    ops.u32(0);
    sid.encode(ops);
    ops.u64(off);
    ops.u64(len);
    auto r = c.run(ops.take().to_bytes(), 0, false);
    V4Client::skip_sequence_res(r.dec);
    V4Client::expect_op(r.dec, kOpPutfh);
    ru32(r.dec);  // opcode
    uint32_t code = ru32(r.dec);
    if (code == 0) return (int)v4::Stateid4::decode(r.dec).seqid;
    return -(int)code;
  };
  if (do_locku(a, lsid2, 0, UINT64_MAX) != 3) fatal("v4lock: LOCKU did not bump seqid");
  if (do_lockt(b, "lo-b", 0, 100) != 0) fatal("v4lock: range still locked after LOCKU");

  // Data survived all the lock churn.
  if (v4_read(a, oa.fh, oa.stateid, payload.size()) != payload)
    fatal("v4lock: data changed under locking");
  if (v4_close(b, ob.fh, ob.stateid) != 0) fatal("v4lock: b CLOSE failed");
  if (v4_close(a, oa.fh, oa.stateid) != 0) fatal("v4lock: a CLOSE failed");
  // CLOSE released a's remaining lock state: b may now take the whole file exclusively.
  V4Client c(host, nfs_port, "lightnfs-accept-v4-c");
  c.establish();
  auto oc = v4_open(c, root, "v4lock.bin", 3, 0, "lock-owner-c", false);
  v4::Stateid4 lc;
  if (do_lock(c, oc.fh, 2, 0, UINT64_MAX, true, oc.stateid, "lo-c", &lc, nullptr) != 0)
    fatal("v4lock: whole-file lock after CLOSE should succeed");
  if (v4_close(c, oc.fh, oc.stateid) != 0) fatal("v4lock: c CLOSE failed");
  (void)backing;
  b.destroy();
  a.destroy();
  c.destroy();
  std::printf("accept_client v4lock OK: LOCK new/existing owner, LOCKT probe, DENIED with "
              "holder, upgrade+LOCKU split, CLOSE releases locks, data intact\n");
  return 0;
}

// Server-restart reclaim scenario (development plan §6.3 / design 07 §7.5): open +
// write, run RESTART_CMD (kills and restarts the server), then re-establish the same
// client identity and CLAIM_PREVIOUS inside grace; verifies data, the STALE_STATEID /
// GRACE / NO_GRACE gates and RECLAIM_COMPLETE's early grace exit.
int cmd_v4reclaim(const char* host, uint16_t nfs_port, const std::string& export_path,
                  const fs::path& backing, const std::string& restart_cmd) {
  using namespace v4;
  std::vector<std::byte> fh;
  Stateid4 old_sid;
  std::string before = "before-restart";
  std::span<const std::byte> before_bytes(
      reinterpret_cast<const std::byte*>(before.data()), before.size());
  {
    V4Client c(host, nfs_port);
    c.establish();
    auto root = lookup_path(c, split_path(export_path));
    auto o = v4_open(c, root, "reclaim.bin", 3, 0, "reclaim-owner", true, 0, 0);
    if (o.status != 0) fatal("v4reclaim: OPEN status %u", o.status);
    if (v4_write(c, o.fh, o.stateid, 0, before_bytes, 2) != 0) fatal("v4reclaim: WRITE failed");
    fh = o.fh;
    old_sid = o.stateid;
    // Connection dropped without CLOSE: the state is live when the server dies.
  }
  std::printf("v4reclaim: state held, restarting server: %s\n", restart_cmd.c_str());
  if (std::system(restart_cmd.c_str()) != 0) fatal("v4reclaim: restart command failed");

  V4Client c(host, nfs_port);
  c.establish(false);  // same co_ownerid + verifier: listed; no RECLAIM_COMPLETE yet
  auto root = lookup_path(c, split_path(export_path));
  uint32_t st = 0;
  (void)v4_read(c, fh, old_sid, before.size(), &st);
  if (st == 70) {  // NFS4ERR_STALE (handle): unprivileged fallback handles on a
                   // filesystem without STATX_BTIME are process-local (design 06);
                   // re-resolve by name so the state-level scenario still runs.
    std::printf("v4reclaim: note: filehandle not stable across restart (fallback handle "
                "mode without btime); re-resolving by name\n");
    auto comps = split_path(export_path);
    comps.push_back("reclaim.bin");
    fh = lookup_path(c, comps);
    (void)v4_read(c, fh, old_sid, before.size(), &st);
  }
  if (st != kStaleStateid) fatal("v4reclaim: old stateid: expected STALE_STATEID, got %u", st);
  auto plain = v4_open(c, root, "reclaim.bin", 3, 0, "reclaim-owner", false);
  if (plain.status != kGrace) fatal("v4reclaim: plain OPEN in grace: expected GRACE, got %u",
                                    plain.status);
  auto re = v4_open(c, fh, "", 3, 0, "reclaim-owner", false, 1);
  if (re.status != 0) fatal("v4reclaim: CLAIM_PREVIOUS status %u", re.status);
  auto data = v4_read(c, re.fh, re.stateid, before.size());
  if (std::string(reinterpret_cast<const char*>(data.data()), data.size()) != before)
    fatal("v4reclaim: data lost across restart");
  std::string after = " after";
  if (v4_write(c, re.fh, re.stateid, before.size(),
               std::span<const std::byte>(reinterpret_cast<const std::byte*>(after.data()),
                                          after.size()), 2) != 0)
    fatal("v4reclaim: WRITE with reclaimed stateid failed");
  if (v4_reclaim_complete(c) != 0) fatal("v4reclaim: RECLAIM_COMPLETE failed");
  plain = v4_open(c, root, "reclaim.bin", 3, 0, "reclaim-owner", false);
  if (plain.status != 0) fatal("v4reclaim: OPEN after grace: status %u", plain.status);
  auto late = v4_open(c, fh, "", 3, 0, "reclaim-owner", false, 1);
  if (late.status != kNoGrace) fatal("v4reclaim: late reclaim: expected NO_GRACE, got %u",
                                     late.status);
  if (v4_close(c, re.fh, plain.stateid) != 0) fatal("v4reclaim: CLOSE failed");
  auto local = read_local(backing / "reclaim.bin");
  if (std::string(reinterpret_cast<const char*>(local.data()), local.size()) != before + after)
    fatal("v4reclaim: backing content wrong after reclaim");
  c.destroy();
  std::printf("accept_client v4reclaim OK: CLAIM_PREVIOUS inside grace, data intact, "
              "STALE_STATEID/GRACE/NO_GRACE gates, early grace exit\n");
  return 0;
}

// ---- multi-gateway failover (design 09, plan 10 E1) ----------------------------------

namespace v4 {
constexpr uint32_t kBadSession = 10052, kDelay = 10008;

// One LOCK op: new lock-owner (open_to_lock_owner4) or existing (exist_lock_owner4),
// with the reclaim flag the grace-period path needs.  Returns the LOCK status; the
// lock stateid on success.
uint32_t v4_lock_op(V4Client& c, const std::vector<std::byte>& fh, uint32_t type,
                    uint64_t off, uint64_t len, bool reclaim, bool new_owner,
                    const Stateid4& sid, const std::string& owner, Stateid4* out) {
  XdrEnc ops(c.rpc.pool);
  c.seq_header(ops, 2);
  ops.u32(kOpPutfh);
  ops.opaque(fh);
  ops.u32(kOpLock);
  ops.u32(type);
  ops.boolean(reclaim);
  ops.u64(off);
  ops.u64(len);
  ops.boolean(new_owner);
  if (new_owner) {
    ops.u32(0);  // open_seqid (unused in 4.1)
    sid.encode(ops);
    ops.u32(0);  // lock_seqid
    ops.u64(c.clientid);
    ops.string(owner);
  } else {
    sid.encode(ops);
    ops.u32(0);
  }
  auto r = c.run(ops.take().to_bytes(), 0, false);
  if (r.status != 0) return r.status;
  V4Client::skip_sequence_res(r.dec);
  V4Client::expect_op(r.dec, kOpPutfh);
  uint32_t opcode = ru32(r.dec);
  uint32_t code = ru32(r.dec);
  if (opcode != kOpLock) fatal("v4: expected LOCK resop, got op=%u status=%u", opcode, code);
  if (code == 0 && out) *out = Stateid4::decode(r.dec);
  return code;
}

// SEQUENCE alone under `sessionid`: the status tells whether that session exists here.
uint32_t v4_probe_session(V4Client& c, const std::array<std::byte, 16>& sessionid) {
  XdrEnc ops(c.rpc.pool);
  ops.u32(0);
  ops.u32(c.minor);
  ops.u32(1);
  ops.u32(kOpSequence);
  ops.opaque_fixed(sessionid);
  ops.u32(1);
  ops.u32(0);
  ops.u32(0);
  ops.boolean(false);
  return c.run(ops.take().to_bytes(), 0, false).status;
}
}  // namespace v4

// Gateway failover with state held (design 09 §9.6, plan 10 E1): gateway A serves an
// OPEN + LOCK + unstable WRITE; `takeover_cmd` kills A and makes B take over (fence,
// epoch+1, shared reclaim list → grace); the client then proves on B that A's session
// is BADSESSION, the identity (server_owner/scope) is the cluster's while the clientid
// carries the new epoch, the old stateid is STALE, CLAIM_PREVIOUS and LOCK(reclaim)
// succeed inside grace (DELAY retried while the storage side lets go, plan 10 B2), the
// write verifier changed so the unstable data is re-sent and byte-verified, and
// RECLAIM_COMPLETE ends grace early.
int cmd_v4failover(const char* host, uint16_t port_a, uint16_t port_b,
                   const std::string& export_path, const fs::path& backing,
                   const std::string& takeover_cmd) {
  using namespace v4;
  const std::string owner = "failover-owner", lock_owner = "failover-lock";
  auto payload = random_bytes(64 * 1024, 0x0f);
  std::vector<std::byte> fh;
  Stateid4 open_sid, lock_sid;
  std::array<std::byte, 16> session_a{};
  std::array<std::byte, 8> verf_a{};
  uint64_t clientid_a = 0;
  std::string owner_a, scope_a;
  {
    V4Client a(host, port_a);
    a.establish();
    clientid_a = a.clientid;
    owner_a = a.server_owner;
    scope_a = a.server_scope;
    session_a = a.sessionid;
    auto root = lookup_path(a, split_path(export_path));
    auto o = v4_open(a, root, "failover.bin", 3, 0, owner, true, 0, 0);
    if (o.status != 0) fatal("v4failover: OPEN(CREATE) on A: status %u", o.status);
    fh = o.fh;
    open_sid = o.stateid;
    if (v4_lock_op(a, fh, 2, 0, 100, false, true, open_sid, lock_owner, &lock_sid) != 0)
      fatal("v4failover: LOCK on A failed");
    if (v4_write(a, fh, lock_sid, 0, payload, 0, &verf_a) != 0)
      fatal("v4failover: WRITE(UNSTABLE) on A failed");
    std::printf("v4failover: A holds open+lock+unstable write: clientid=%#llx owner=%s "
                "scope=%s\n",
                (unsigned long long)clientid_a, owner_a.c_str(), scope_a.c_str());
    // Connection dropped without CLOSE/LOCKU: the state is live when A dies.
  }
  std::printf("v4failover: taking over: %s\n", takeover_cmd.c_str());
  auto t0 = std::chrono::steady_clock::now();
  if (std::system(takeover_cmd.c_str()) != 0) fatal("v4failover: takeover command failed");
  auto takeover_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0)
                         .count();

  V4Client b(host, port_b);
  // A's session is unknown to B: BADSESSION, not a hang or a stale reply.
  uint32_t probe = v4_probe_session(b, session_a);
  if (probe != kBadSession)
    fatal("v4failover: old session on B: expected BADSESSION, got %u", probe);
  b.establish(false);  // same co_ownerid + verifier: listed by A → no RECLAIM_COMPLETE yet
  if (b.server_owner != owner_a || b.server_scope != scope_a)
    fatal("v4failover: identity changed across takeover: owner %s→%s scope %s→%s",
          owner_a.c_str(), b.server_owner.c_str(), scope_a.c_str(), b.server_scope.c_str());
  if ((b.clientid >> 32) == (clientid_a >> 32))
    fatal("v4failover: clientid epoch did not advance (%#llx → %#llx)",
          (unsigned long long)clientid_a, (unsigned long long)b.clientid);
  std::printf("v4failover: B identity kept, clientid %#llx → %#llx (epoch %llu → %llu)\n",
              (unsigned long long)clientid_a, (unsigned long long)b.clientid,
              (unsigned long long)(clientid_a >> 32), (unsigned long long)(b.clientid >> 32));

  auto root = lookup_path(b, split_path(export_path));
  uint32_t st = 0;
  (void)v4_read(b, fh, open_sid, 16, &st);
  if (st == 70) {  // NFS4ERR_STALE handle: fallback handles without btime (design 06)
    std::printf("v4failover: note: filehandle not stable across gateways (fallback "
                "handle mode without btime); re-resolving by name\n");
    auto comps = split_path(export_path);
    comps.push_back("failover.bin");
    fh = lookup_path(b, comps);
    (void)v4_read(b, fh, open_sid, 16, &st);
  }
  if (st != kStaleStateid) fatal("v4failover: old stateid on B: expected STALE_STATEID, got %u", st);
  auto plain = v4_open(b, root, "failover.bin", 3, 0, owner, false);
  if (plain.status != kGrace)
    fatal("v4failover: plain OPEN in grace: expected GRACE, got %u", plain.status);
  auto re = v4_open(b, fh, "", 3, 0, owner, false, 1);
  if (re.status != 0) fatal("v4failover: OPEN(CLAIM_PREVIOUS) on B: status %u", re.status);
  // LOCK(reclaim): DELAY while the failed gateway's lock is still held on the
  // storage side (plan 10 B2) — retry inside grace, count the retries.
  Stateid4 re_lock;
  unsigned retries = 0;
  for (;;) {
    uint32_t code = v4_lock_op(b, re.fh, 2, 0, 100, true, true, re.stateid, lock_owner, &re_lock);
    if (code == 0) break;
    if (code != kDelay) fatal("v4failover: LOCK(reclaim) on B: status %u", code);
    if (++retries > 200) fatal("v4failover: LOCK(reclaim) still DELAY after %u retries", retries);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  // The unstable write may not have reached the backing tree: COMMIT reports a new
  // verifier (new epoch), so the client re-sends the data FILE_SYNC.
  auto verf_b = v4_commit(b, re.fh);
  if (verf_b == verf_a) fatal("v4failover: write verifier unchanged across takeover");
  if (v4_write(b, re.fh, re_lock, 0, payload, 2) != 0)
    fatal("v4failover: WRITE(FILE_SYNC) with reclaimed lock stateid failed");
  auto back = v4_read(b, re.fh, re_lock, payload.size());
  if (back != payload) fatal("v4failover: READ after re-send differs from the payload");
  if (read_local(backing / "failover.bin") != payload)
    fatal("v4failover: backing content differs from the payload");
  if (v4_reclaim_complete(b) != 0) fatal("v4failover: RECLAIM_COMPLETE failed");
  plain = v4_open(b, root, "failover.bin", 3, 0, owner, false);
  if (plain.status != 0) fatal("v4failover: OPEN after RECLAIM_COMPLETE: status %u", plain.status);
  if (v4_close(b, re.fh, plain.stateid) != 0) fatal("v4failover: CLOSE failed");
  b.destroy();
  std::printf("accept_client v4failover OK: takeover %lld ms, BADSESSION/STALE_STATEID/GRACE "
              "gates, CLAIM_PREVIOUS + LOCK(reclaim) (%u DELAY retries), verifier changed, "
              "data re-sent and verified, early grace exit\n",
              (long long)takeover_ms, retries);
  return 0;
}

// Lease-expiry scenario (development plan §6.3 / design 07 §7.4): a client holding a
// deny-WRITE open vanishes (connection dropped, no CLOSE).  Inside the lease a second
// client is SHARE_DENIED; once the lease lapses the holder is a courtesy client and the
// conflicting OPEN reclaims it.  A second vanished holder is left for the timeout
// path, which the caller verifies through lightnfs-ctl (reclaim_timeout counter).
int cmd_v4courtesy(const char* host, uint16_t nfs_port, const std::string& export_path,
                   unsigned lease_seconds) {
  using namespace v4;
  auto hold = [&](const char* name, const char* owner) {
    V4Client holder(host, nfs_port, std::string("lightnfs-accept-") + owner);
    holder.establish();
    auto root = lookup_path(holder, split_path(export_path));
    auto o = v4_open(holder, root, name, 1, 2, owner, true, 0, 0);  // READ, deny WRITE
    if (o.status != 0) fatal("v4courtesy: holder OPEN status %u", o.status);
    // holder goes out of scope: TCP closed, session/state left behind
  };
  hold("courtesy.bin", "holder-a");
  V4Client b(host, nfs_port, "lightnfs-accept-v4-b");
  b.establish();
  auto root = lookup_path(b, split_path(export_path));
  auto denied = v4_open(b, root, "courtesy.bin", 2, 0, "b-owner", false);
  if (denied.status != kShareDenied)
    fatal("v4courtesy: inside lease: expected SHARE_DENIED, got %u", denied.status);
  std::printf("v4courtesy: denied inside lease; waiting %us for expiry\n", lease_seconds + 2);
  for (unsigned i = 0; i < lease_seconds + 2; ++i) {
    sleep(1);
    XdrEnc hb(b.rpc.pool);  // keep b's own lease alive
    b.seq_header(hb, 0);
    (void)b.run(hb.take().to_bytes());
  }
  auto ok = v4_open(b, root, "courtesy.bin", 2, 0, "b-owner", false);
  if (ok.status != 0) fatal("v4courtesy: after lease: expected OK (conflict reclaim), got %u",
                            ok.status);
  std::string payload = "written after courtesy reclaim";
  if (v4_write(b, ok.fh, ok.stateid, 0,
               std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()),
                                          payload.size()), 2) != 0)
    fatal("v4courtesy: WRITE failed");
  if (v4_close(b, ok.fh, ok.stateid) != 0) fatal("v4courtesy: CLOSE failed");
  // Leave a second vanished holder for the timeout path.
  hold("timeout.bin", "holder-c");
  b.destroy();
  std::printf("accept_client v4courtesy OK: SHARE_DENIED inside lease, conflict reclaim "
              "after expiry; timeout holder left behind\n");
  return 0;
}

// Full v4.1 read-only acceptance: session, pseudo-root navigation, recursive walk with
// byte verification, exactly-once slot replay, negative checks.
int cmd_v4walk(const char* host, uint16_t nfs_port, const std::string& export_path,
               const fs::path& backing) {
  v4::V4Client c(host, nfs_port);

  // minorversion 0 must be rejected before anything else.
  {
    XdrEnc bad(c.rpc.pool);
    bad.u32(0);
    bad.u32(0);  // minorversion 0
    bad.u32(1);
    bad.u32(v4::kOpPutrootfh);
    auto r = c.run(bad.take().to_bytes(), 10021);  // MINOR_VERS_MISMATCH
    (void)r;
  }

  c.establish();

  // Navigate the pseudo root down the export path components.
  std::vector<std::string> components;
  {
    std::string rest = export_path;
    size_t pos = 0;
    while (pos < rest.size()) {
      while (pos < rest.size() && rest[pos] == '/') ++pos;
      size_t end = rest.find('/', pos);
      if (pos < rest.size())
        components.push_back(rest.substr(pos, end == std::string::npos ? std::string::npos
                                                                       : end - pos));
      if (end == std::string::npos) break;
      pos = end + 1;
    }
  }
  auto export_root = v4::lookup_path(c, components);

  size_t dirs = 0, files = 0, bytes = 0;
  v4::v4_walk_dir(c, export_root, backing, dirs, files, bytes);

  // Exactly-once: a verbatim retransmission (same xid, same slot/seq) must be
  // answered with the original reply bytes.
  {
    XdrEnc ops(c.rpc.pool);
    c.seq_header(ops, 2, true);
    ops.u32(v4::kOpPutrootfh);
    ops.u32(v4::kOpGetfh);
    auto body = ops.take().to_bytes();
    uint32_t xid = c.rpc.next_xid++;
    auto record = build_call(c.rpc.pool, xid, kNfsProg, 1, body, 4);
    send_record(c.rpc.fd, record);
    auto first = read_record(c.rpc.fd);
    send_record(c.rpc.fd, record);  // byte-identical retransmission
    auto second = read_record(c.rpc.fd);
    if (first != second) fatal("v4: slot replay bytes differ");
  }

  c.destroy();
  std::printf("accept_client v4walk OK: %zu dirs, %zu files (%zu bytes verified) via "
              "vers=4.1 sessions; pseudo-root crossing, slot replay and negative "
              "checks passed\n", dirs, files, bytes);
  std::printf("v4walk: server_owner=%s server_scope=%s\n", c.server_owner.c_str(),
              c.server_scope.c_str());
  return 0;
}

// ---------- NFSv4.2 sweets acceptance (phase 6) ----------

namespace v4 {

constexpr uint32_t kOpAllocate = 59, kOpCopy = 60, kOpDeallocate = 62, kOpSeek = 69,
                   kOpClone = 71;
[[maybe_unused]] constexpr uint32_t kOpIllegal = 10044;
constexpr uint32_t kNxio = 6, kNotsupp = 10004, kOpIllegalStatus = 10044, kInval = 22;

struct SeekOut {
  uint32_t status = 0;
  bool eof = false;
  uint64_t offset = 0;
};
SeekOut v4_seek(V4Client& c, const std::vector<std::byte>& fh, const Stateid4& sid,
                uint64_t offset, uint32_t what) {
  XdrEnc ops(c.rpc.pool);
  c.seq_header(ops, 2);
  ops.u32(kOpPutfh);
  ops.opaque(fh);
  ops.u32(kOpSeek);
  sid.encode(ops);
  ops.u64(offset);
  ops.u32(what);
  auto r = c.run(ops.take().to_bytes(), 0, false);
  SeekOut out;
  out.status = r.status;
  if (r.status != 0) return out;
  V4Client::skip_sequence_res(r.dec);
  V4Client::expect_op(r.dec, kOpPutfh);
  V4Client::expect_op(r.dec, kOpSeek);
  out.eof = rbool(r.dec);
  out.offset = ru64(r.dec);
  return out;
}

uint32_t v4_alloc(V4Client& c, const std::vector<std::byte>& fh, const Stateid4& sid,
                  uint32_t op, uint64_t offset, uint64_t length) {
  XdrEnc ops(c.rpc.pool);
  c.seq_header(ops, 2);
  ops.u32(kOpPutfh);
  ops.opaque(fh);
  ops.u32(op);
  sid.encode(ops);
  ops.u64(offset);
  ops.u64(length);
  return c.run(ops.take().to_bytes(), 0, false).status;
}

// {PUTFH src, SAVEFH, PUTFH dst, COPY|CLONE}; COPY returns the byte count via *count.
uint32_t v4_copy(V4Client& c, const std::vector<std::byte>& src, const Stateid4& ssid,
                 const std::vector<std::byte>& dst, const Stateid4& dsid, uint64_t soff,
                 uint64_t doff, uint64_t count, bool clone, uint64_t* copied = nullptr) {
  XdrEnc ops(c.rpc.pool);
  c.seq_header(ops, 4);
  ops.u32(kOpPutfh);
  ops.opaque(src);
  ops.u32(kOpSavefh);
  ops.u32(kOpPutfh);
  ops.opaque(dst);
  ops.u32(clone ? kOpClone : kOpCopy);
  ssid.encode(ops);
  dsid.encode(ops);
  ops.u64(soff);
  ops.u64(doff);
  ops.u64(count);
  if (!clone) {
    ops.boolean(true);
    ops.boolean(true);
    ops.u32(0);
  }
  auto r = c.run(ops.take().to_bytes(), 0, false);
  if (r.status != 0) return r.status;
  V4Client::skip_sequence_res(r.dec);
  V4Client::expect_op(r.dec, kOpPutfh);
  V4Client::expect_op(r.dec, kOpSavefh);
  V4Client::expect_op(r.dec, kOpPutfh);
  V4Client::expect_op(r.dec, clone ? kOpClone : kOpCopy);
  if (!clone) {
    if (ru32(r.dec) != 0) fatal("v42: COPY answered with a callback stateid");
    uint64_t n = ru64(r.dec);
    (void)ru32(r.dec);            // committed
    (void)r.dec.opaque_fixed(8);  // verifier
    if (!rbool(r.dec) || !rbool(r.dec)) fatal("v42: COPY not consecutive+synchronous");
    if (copied) *copied = n;
  }
  return 0;
}

}  // namespace v4

// Loopback v4.2 scenario against a live server: SEEK/ALLOCATE/DEALLOCATE mirrored on the
// backing file (size, zeroes, lseek(SEEK_HOLE) agreement), whole-file + ranged COPY
// byte-verified, CLONE honored or NOTSUPP (fs without reflink), the 4.2 opcodes ILLEGAL
// at minorversion 1.
int cmd_v42(const char* host, uint16_t nfs_port, const std::string& export_path,
            const fs::path& backing) {
  using namespace v4;
  V4Client a(host, nfs_port, "lightnfs-accept-v42");
  a.minor = 2;
  a.establish();
  auto root = lookup_path(a, split_path(export_path));
  const uint64_t blk = 1 << 16;
  auto block = random_bytes(blk, 0x42);

  auto src = v4_open(a, root, "v42src.bin", 3, 0, "v42-owner", true);
  if (src.status != 0) fatal("v42: OPEN(CREATE) src status %u", src.status);
  if (v4_write(a, src.fh, src.stateid, 0, block, 2) != 0) fatal("v42: WRITE 0 failed");
  if (v4_write(a, src.fh, src.stateid, 2 * blk, block, 2) != 0) fatal("v42: WRITE 2 failed");

  // DEALLOCATE the middle block: size unchanged, zeroes on the backing file.
  if (uint32_t st = v4_alloc(a, src.fh, src.stateid, kOpDeallocate, blk, blk); st != 0)
    fatal("v42: DEALLOCATE status %u", st);
  if (fs::file_size(backing / "v42src.bin") != 3 * blk) fatal("v42: DEALLOCATE changed size");
  {
    auto on_disk = read_local(backing / "v42src.bin");
    std::vector<std::byte> expect(3 * blk);
    std::copy(block.begin(), block.end(), expect.begin());
    std::copy(block.begin(), block.end(), expect.begin() + 2 * blk);
    if (on_disk != expect) fatal("v42: backing content after DEALLOCATE mismatch");
  }
  // SEEK agrees with lseek(2) on the backing file.
  {
    int fd = ::open((backing / "v42src.bin").c_str(), O_RDONLY);
    if (fd < 0) fatal("v42: cannot open backing file");
    off_t local_hole = ::lseek(fd, 0, SEEK_HOLE);
    off_t local_data = ::lseek(fd, (off_t)blk, SEEK_DATA);
    ::close(fd);
    auto hole = v4_seek(a, src.fh, src.stateid, 0, 1);
    if (hole.status != 0) fatal("v42: SEEK hole status %u", hole.status);
    if (hole.eof || hole.offset != (uint64_t)local_hole)
      fatal("v42: SEEK hole %llu eof=%d vs lseek %lld", (unsigned long long)hole.offset,
            hole.eof, (long long)local_hole);
    auto data = v4_seek(a, src.fh, src.stateid, blk, 0);
    if (data.status != 0) fatal("v42: SEEK data status %u", data.status);
    if (data.offset != (uint64_t)local_data)
      fatal("v42: SEEK data %llu vs lseek %lld", (unsigned long long)data.offset,
            (long long)local_data);
    auto tail = v4_seek(a, src.fh, src.stateid, 2 * blk + 1, 1);
    if (tail.status != 0 || !tail.eof || tail.offset != 3 * blk)
      fatal("v42: SEEK hole at tail: status %u eof=%d off=%llu", tail.status, tail.eof,
            (unsigned long long)tail.offset);
    if (v4_seek(a, src.fh, src.stateid, 3 * blk, 0).status != kNxio)
      fatal("v42: SEEK data past EOF expected NXIO");
  }
  // ALLOCATE past EOF extends; zero length is INVAL.
  if (uint32_t st = v4_alloc(a, src.fh, src.stateid, kOpAllocate, 3 * blk, 4096); st != 0)
    fatal("v42: ALLOCATE status %u", st);
  if (fs::file_size(backing / "v42src.bin") != 3 * blk + 4096) fatal("v42: ALLOCATE size");
  if (v4_alloc(a, src.fh, src.stateid, kOpAllocate, 0, 0) != kInval)
    fatal("v42: ALLOCATE length 0 expected INVAL");
  if (v4_setattr_size(a, src.fh, src.stateid, 3 * blk) != 0) fatal("v42: truncate back");

  // COPY whole file, then a ranged copy at an offset; byte-verified on the backing tree.
  auto dst = v4_open(a, root, "v42dst.bin", 3, 0, "v42-owner", true);
  if (dst.status != 0) fatal("v42: OPEN(CREATE) dst status %u", dst.status);
  uint64_t copied = 0;
  if (uint32_t st = v4_copy(a, src.fh, src.stateid, dst.fh, dst.stateid, 0, 0, 0, false, &copied);
      st != 0)
    fatal("v42: COPY status %u", st);
  if (copied != 3 * blk) fatal("v42: COPY count %llu", (unsigned long long)copied);
  (void)v4_commit(a, dst.fh);
  if (read_local(backing / "v42dst.bin") != read_local(backing / "v42src.bin"))
    fatal("v42: COPY content mismatch");
  if (uint32_t st = v4_copy(a, src.fh, src.stateid, dst.fh, dst.stateid, 2 * blk, 3 * blk, 100,
                            false, &copied);
      st != 0 || copied != 100)
    fatal("v42: ranged COPY status %u count %llu", st, (unsigned long long)copied);
  {
    auto d = read_local(backing / "v42dst.bin");
    if (d.size() != 3 * blk + 100 ||
        !std::equal(d.begin() + 3 * blk, d.end(), block.begin()))
      fatal("v42: ranged COPY content mismatch");
  }
  // Read-through-server agrees with the backing file (cache coherence after COPY).
  if (v4_read(a, dst.fh, dst.stateid, 3 * blk + 100) != read_local(backing / "v42dst.bin"))
    fatal("v42: READ after COPY mismatch");

  // CLONE: reflink when the export fs supports it, NOTSUPP otherwise.
  auto cl = v4_open(a, root, "v42clone.bin", 3, 0, "v42-owner", true);
  if (cl.status != 0) fatal("v42: OPEN(CREATE) clone status %u", cl.status);
  uint32_t cst = v4_copy(a, src.fh, src.stateid, cl.fh, cl.stateid, 0, 0, 0, true);
  const char* clone_note = "";
  if (cst == 0) {
    if (read_local(backing / "v42clone.bin") != read_local(backing / "v42src.bin"))
      fatal("v42: CLONE content mismatch");
    clone_note = "CLONE reflinked";
  } else if (cst == kNotsupp) {
    clone_note = "CLONE NOTSUPP (export fs without reflink)";
  } else {
    fatal("v42: CLONE status %u", cst);
  }

  // Stateid discipline: read-only open may SEEK, not ALLOCATE.
  auto ro = v4_open(a, root, "v42src.bin", 1, 0, "v42-ro", false);
  if (ro.status != 0) fatal("v42: ro OPEN status %u", ro.status);
  if (v4_seek(a, src.fh, ro.stateid, 0, 0).status != 0) fatal("v42: ro SEEK failed");
  if (v4_alloc(a, src.fh, ro.stateid, kOpAllocate, 0, 1) != kOpenmode)
    fatal("v42: ro ALLOCATE expected OPENMODE");
  if (v4_close(a, src.fh, ro.stateid) != 0) fatal("v42: CLOSE ro");
  if (v4_close(a, cl.fh, cl.stateid) != 0) fatal("v42: CLOSE clone");
  if (v4_close(a, dst.fh, dst.stateid) != 0) fatal("v42: CLOSE dst");
  if (v4_close(a, src.fh, src.stateid) != 0) fatal("v42: CLOSE src");
  a.destroy();

  // minorversion 1 on the same server: the 4.2 opcodes are OP_ILLEGAL.
  V4Client b(host, nfs_port, "lightnfs-accept-v41");
  b.establish();
  auto root1 = lookup_path(b, split_path(export_path));
  auto f1 = v4_open(b, root1, "v42src.bin", 1, 0, "v41-owner", false);
  if (f1.status != 0) fatal("v42: 4.1 OPEN status %u", f1.status);
  if (v4_seek(b, f1.fh, f1.stateid, 0, 0).status != kOpIllegalStatus)
    fatal("v42: SEEK at minorversion 1 expected OP_ILLEGAL");
  if (v4_close(b, f1.fh, f1.stateid) != 0) fatal("v42: 4.1 CLOSE");
  b.destroy();

  std::printf("accept_client v42 OK: DEALLOCATE/SEEK/ALLOCATE mirrored on backing, COPY "
              "%llu+100 bytes byte-verified, %s, OPENMODE/INVAL/NXIO discipline, 4.2 ops "
              "ILLEGAL at minor 1\n",
              (unsigned long long)(3 * blk), clone_note);
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
                 "       accept_client connstorm HOST NFS_PORT MOUNT_PORT COUNT PIPELINE\n"
                 "       accept_client v4walk HOST NFS_PORT MOUNT_PORT EXPORT BACKING\n"
                 "       accept_client v4rw   HOST NFS_PORT MOUNT_PORT EXPORT BACKING\n"
                 "       accept_client v4reclaim HOST NFS_PORT MOUNT_PORT EXPORT BACKING "
                 "RESTART_CMD\n"
                 "       accept_client v4courtesy HOST NFS_PORT MOUNT_PORT EXPORT LEASE_SECS\n"
                 "       accept_client v4failover HOST PORT_A PORT_B EXPORT BACKING "
                 "TAKEOVER_CMD\n"
                 "       accept_client v4lock HOST NFS_PORT MOUNT_PORT EXPORT BACKING\n"
                 "       accept_client v42    HOST NFS_PORT MOUNT_PORT EXPORT BACKING\n"
                 "       accept_client fsync-eio HOST NFS_PORT MOUNT_PORT EXPORT\n");
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
  if (cmd == "v4walk" && argc == 7)
    return cmd_v4walk(host, nfs_port, export_path, argv[6]);
  if (cmd == "v4rw" && argc == 7)
    return cmd_v4rw(host, nfs_port, export_path, argv[6]);
  if (cmd == "v4reclaim" && argc == 8)
    return cmd_v4reclaim(host, nfs_port, export_path, argv[6], argv[7]);
  if (cmd == "v4courtesy" && argc == 7)
    return cmd_v4courtesy(host, nfs_port, export_path, (unsigned)atoi(argv[6]));
  if (cmd == "v4failover" && argc == 8)  // argv[4] is gateway B's port, not a mount port
    return cmd_v4failover(host, nfs_port, mount_port, export_path, argv[6], argv[7]);
  if (cmd == "v4lock" && argc == 7)
    return cmd_v4lock(host, nfs_port, export_path, argv[6]);
  if (cmd == "v42" && argc == 7)
    return cmd_v42(host, nfs_port, export_path, argv[6]);
  if (cmd == "fsync-eio" && argc == 6)
    return cmd_fsync_eio(host, nfs_port, mount_port, export_path);
  return usage();
}
