#include "core/file_handle.hpp"

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>

namespace lnfs::core {
namespace {

uint64_t rotl(uint64_t value, int bits) { return (value << bits) | (value >> (64 - bits)); }

uint64_t read64(const std::byte* p) {
  uint64_t out;
  std::memcpy(&out, p, 8);
  return out;
}

void sip_round(uint64_t& v0, uint64_t& v1, uint64_t& v2, uint64_t& v3) {
  v0 += v1;
  v1 = rotl(v1, 13) ^ v0;
  v0 = rotl(v0, 32);
  v2 += v3;
  v3 = rotl(v3, 16) ^ v2;
  v0 += v3;
  v3 = rotl(v3, 21) ^ v0;
  v2 += v1;
  v1 = rotl(v1, 17) ^ v2;
  v2 = rotl(v2, 32);
}

uint32_t load_be32(const std::byte* p) {
  return (uint32_t(static_cast<uint8_t>(p[0])) << 24) |
         (uint32_t(static_cast<uint8_t>(p[1])) << 16) |
         (uint32_t(static_cast<uint8_t>(p[2])) << 8) |
         uint32_t(static_cast<uint8_t>(p[3]));
}

}  // namespace

Result<FileHandleCodec> FileHandleCodec::load_or_create(const std::string& state_dir) {
  std::error_code ec;
  std::filesystem::create_directories(state_dir, ec);
  if (ec) return Err(errno_from(ec.value()));
  std::string path = state_dir + "/hmac.key";
  std::array<std::byte, 16> key{};
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ssize_t n = ::read(fd, key.data(), key.size());
    int e = errno;
    ::close(fd);
    if (n != static_cast<ssize_t>(key.size())) return Err(errno_from(n < 0 ? e : EINVAL));
    return FileHandleCodec(key);
  }
  if (errno != ENOENT) return Err(errno_from(errno));
  ssize_t n = getrandom(key.data(), key.size(), 0);
  if (n != static_cast<ssize_t>(key.size())) return Err(errno_from(errno ? errno : EIO));
  fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    if (errno == EEXIST) return load_or_create(state_dir);
    return Err(errno_from(errno));
  }
  ssize_t written = ::write(fd, key.data(), key.size());
  int e = errno;
  if (written == static_cast<ssize_t>(key.size())) (void)::fsync(fd);
  ::close(fd);
  if (written != static_cast<ssize_t>(key.size())) return Err(errno_from(e ? e : EIO));
  return FileHandleCodec(key);
}

uint64_t FileHandleCodec::tag(std::span<const std::byte> bytes) const {
  uint64_t k0 = read64(key_.data());
  uint64_t k1 = read64(key_.data() + 8);
  uint64_t v0 = 0x736f6d6570736575ull ^ k0;
  uint64_t v1 = 0x646f72616e646f6dull ^ k1;
  uint64_t v2 = 0x6c7967656e657261ull ^ k0;
  uint64_t v3 = 0x7465646279746573ull ^ k1;
  size_t pos = 0;
  while (pos + 8 <= bytes.size()) {
    uint64_t m = read64(bytes.data() + pos);
    v3 ^= m;
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    v0 ^= m;
    pos += 8;
  }
  uint64_t last = static_cast<uint64_t>(bytes.size()) << 56;
  for (size_t i = 0; pos + i < bytes.size(); ++i)
    last |= uint64_t(static_cast<uint8_t>(bytes[pos + i])) << (8 * i);
  v3 ^= last;
  sip_round(v0, v1, v2, v3);
  sip_round(v0, v1, v2, v3);
  v0 ^= last;
  v2 ^= 0xff;
  for (int i = 0; i < 4; ++i) sip_round(v0, v1, v2, v3);
  return v0 ^ v1 ^ v2 ^ v3;
}

std::vector<std::byte> FileHandleCodec::encode(const ExportEntry& exp,
                                                const backend::ObjId& oid) const {
  std::vector<std::byte> out;
  out.reserve(13 + oid.len);
  out.push_back(static_cast<std::byte>(kVersion));
  out.push_back(static_cast<std::byte>(exp.fsid >> 24));
  out.push_back(static_cast<std::byte>(exp.fsid >> 16));
  out.push_back(static_cast<std::byte>(exp.fsid >> 8));
  out.push_back(static_cast<std::byte>(exp.fsid));
  out.insert(out.end(), oid.view().begin(), oid.view().end());
  uint64_t auth = tag(out);
  const auto* bytes = reinterpret_cast<const std::byte*>(&auth);
  out.insert(out.end(), bytes, bytes + sizeof(auth));
  return out;
}

Result<DecodedHandle> FileHandleCodec::decode(std::span<const std::byte> fh,
                                               const sockaddr_storage& peer) const {
  if (!exports_ || fh.size() < 14 || fh.size() > 64 ||
      static_cast<uint8_t>(fh[0]) != kVersion)
    return Err(Errno::kBadHandle);
  uint64_t expected = tag(fh.first(fh.size() - 8));
  uint64_t supplied = read64(fh.data() + fh.size() - 8);
  uint64_t diff = expected ^ supplied;
  if (diff != 0) return Err(Errno::kBadHandle);
  uint32_t fsid = load_be32(fh.data() + 1);
  ExportEntry* exp = exports_->by_fsid(fsid);
  if (!exp) return Err(errno_from(ESTALE));
  if (!exports_->check_client(peer, *exp)) return Err(errno_from(EACCES));
  auto oid = backend::ObjId::from(fh.subspan(5, fh.size() - 13));
  if (!oid) return Err(Errno::kBadHandle);
  return DecodedHandle{exp, *oid};
}

std::vector<std::byte> FileHandleCodec::encode_raw(uint32_t fsid,
                                                   const backend::ObjId& oid) const {
  std::vector<std::byte> out;
  out.reserve(13 + oid.len);
  out.push_back(static_cast<std::byte>(kVersion));
  out.push_back(static_cast<std::byte>(fsid >> 24));
  out.push_back(static_cast<std::byte>(fsid >> 16));
  out.push_back(static_cast<std::byte>(fsid >> 8));
  out.push_back(static_cast<std::byte>(fsid));
  out.insert(out.end(), oid.view().begin(), oid.view().end());
  uint64_t auth = tag(out);
  const auto* bytes = reinterpret_cast<const std::byte*>(&auth);
  out.insert(out.end(), bytes, bytes + sizeof(auth));
  return out;
}

Result<FileHandleCodec::DecodedV4> FileHandleCodec::decode_v4(
    std::span<const std::byte> fh, const sockaddr_storage& peer) const {
  if (fh.size() < 14 || fh.size() > 64 || static_cast<uint8_t>(fh[0]) != kVersion)
    return Err(Errno::kBadHandle);
  if (tag(fh.first(fh.size() - 8)) != read64(fh.data() + fh.size() - 8))
    return Err(Errno::kBadHandle);
  DecodedV4 out;
  out.fsid = load_be32(fh.data() + 1);
  auto oid = backend::ObjId::from(fh.subspan(5, fh.size() - 13));
  if (!oid) return Err(Errno::kBadHandle);
  out.oid = *oid;
  if (out.fsid == 0) return out;  // pseudo-fs: no export gate here
  if (!exports_) return Err(errno_from(ESTALE));
  out.exp = exports_->by_fsid(out.fsid);
  if (!out.exp) return Err(errno_from(ESTALE));
  if (!exports_->check_client(peer, *out.exp)) return Err(errno_from(EACCES));
  return out;
}

Result<FileHandleCodec::Inspection> FileHandleCodec::inspect(
    std::span<const std::byte> fh) const {
  if (fh.size() < 14 || fh.size() > 64) return Err(Errno::kBadHandle);
  Inspection out;
  out.version = static_cast<uint8_t>(fh[0]);
  out.fsid = load_be32(fh.data() + 1);
  auto oid = backend::ObjId::from(fh.subspan(5, fh.size() - 13));
  if (!oid) return Err(Errno::kBadHandle);
  out.oid = *oid;
  out.hmac_ok = tag(fh.first(fh.size() - 8)) == read64(fh.data() + fh.size() - 8);
  return out;
}

}  // namespace lnfs::core
