#include "backend/api.hpp"

#include <algorithm>
#include <cerrno>
#include <mutex>

namespace lnfs::backend {

Result<ObjId> ObjId::from(std::span<const std::byte> value) {
  if (value.empty() || value.size() > kMax) return Err(errno_from(EINVAL));
  ObjId out;
  out.len = static_cast<uint8_t>(value.size());
  std::copy(value.begin(), value.end(), out.bytes.begin());
  return out;
}

size_t ObjIdHash::operator()(const ObjId& id) const noexcept {
  // FNV-1a is sufficient for in-process sharding/maps; file handles are authenticated
  // separately with SipHash.
  size_t h = sizeof(size_t) == 8 ? 1469598103934665603ull : 2166136261u;
  for (std::byte b : id.view()) {
    h ^= static_cast<uint8_t>(b);
    h *= sizeof(size_t) == 8 ? 1099511628211ull : 16777619u;
  }
  return h;
}

bool Cred::in_group(uint32_t group) const {
  return gid == group || std::find(gids.begin(), gids.end(), group) != gids.end();
}

namespace {
template <class T>
rt::Task<Result<T>> unsupported() {
  co_return Err(errno_from(EOPNOTSUPP));
}
}  // namespace

rt::Task<Result<Attr>> Object::setattr(const Cred&, const SetAttr&) {
  return unsupported<Attr>();
}

rt::Task<Result<AccessMask>> Object::access(const Cred& cred, AccessMask want) {
  auto got = co_await getattr();
  if (!got) co_return Err(got.error());
  const Attr& a = *got;
  if (cred.uid == 0) co_return want;
  uint32_t shift = cred.uid == a.uid ? 6 : (cred.in_group(a.gid) ? 3 : 0);
  uint32_t bits = (a.mode >> shift) & 7;
  AccessMask allowed;
  if (bits & 4) allowed.set(Access::kRead);
  if (bits & 2) {
    allowed.set(Access::kModify).set(Access::kExtend);
    if (a.type == FType::kDir) allowed.set(Access::kDelete);
  }
  if (bits & 1) {
    if (a.type == FType::kDir) allowed.set(Access::kLookup);
    else allowed.set(Access::kExecute);
  }
  co_return allowed & want;
}

rt::Task<Result<ObjPtr>> Object::lookup(const Cred&, std::string_view) {
  return unsupported<ObjPtr>();
}
rt::Task<Result<Created>> Object::create(const Cred&, std::string_view, const SetAttr&,
                                          ExclVerf*) {
  return unsupported<Created>();
}
rt::Task<Result<Created>> Object::mkdir(const Cred&, std::string_view, const SetAttr&) {
  return unsupported<Created>();
}
rt::Task<Result<Created>> Object::symlink(const Cred&, std::string_view, std::string_view,
                                           const SetAttr&) {
  return unsupported<Created>();
}
rt::Task<Result<Created>> Object::mknod(const Cred&, std::string_view, FType, DevT,
                                         const SetAttr&) {
  return unsupported<Created>();
}
rt::Task<Result<void>> Object::unlink(const Cred&, std::string_view) {
  return unsupported<void>();
}
rt::Task<Result<void>> Object::rmdir(const Cred&, std::string_view) {
  return unsupported<void>();
}
rt::Task<Result<void>> Object::rename(const Cred&, std::string_view, Object&,
                                       std::string_view) {
  return unsupported<void>();
}
rt::Task<Result<void>> Object::link(const Cred&, Object&, std::string_view) {
  return unsupported<void>();
}
rt::Task<Result<DirPage>> Object::readdir(const Cred&, uint64_t, uint32_t) {
  return unsupported<DirPage>();
}
rt::Task<Result<std::string>> Object::readlink() { return unsupported<std::string>(); }
rt::Task<Result<OpenPtr>> Object::open(const Cred&, OpenFlags) { return unsupported<OpenPtr>(); }
rt::Task<Result<uint32_t>> Object::read(OpenCtx, uint64_t, std::span<std::byte>, bool&) {
  return unsupported<uint32_t>();
}
rt::Task<Result<uint32_t>> Object::write(OpenCtx, uint64_t, std::span<const std::byte>,
                                          Stability) {
  return unsupported<uint32_t>();
}
rt::Task<Result<void>> Object::commit(OpenCtx, uint64_t, uint64_t) {
  return unsupported<void>();
}
rt::Task<Result<uint64_t>> Object::seek(OpenCtx, uint64_t, SeekWhat) {
  return unsupported<uint64_t>();
}
rt::Task<Result<void>> Object::allocate(OpenCtx, uint64_t, uint64_t) {
  return unsupported<void>();
}
rt::Task<Result<void>> Object::deallocate(OpenCtx, uint64_t, uint64_t) {
  return unsupported<void>();
}
rt::Task<Result<void>> Object::clone(OpenCtx, Object&, OpenCtx, uint64_t, uint64_t,
                                     uint64_t) {
  return unsupported<void>();
}
rt::Task<Result<uint64_t>> Object::copy_range(OpenCtx, Object&, OpenCtx, uint64_t,
                                               uint64_t, uint64_t) {
  return unsupported<uint64_t>();
}

rt::Task<Result<void>> Backend::start() { co_return Result<void>{}; }
rt::Task<Result<void>> Backend::stop() { co_return Result<void>{}; }

namespace {
std::mutex& registry_mutex() {
  static std::mutex value;
  return value;
}
std::vector<BackendFactory>& registry() {
  static std::vector<BackendFactory> value;
  return value;
}
}  // namespace

void register_backend(BackendFactory factory) {
  if (!factory.make || factory.name.empty() || factory.api_version != kBackendApiVersion)
    return;
  std::lock_guard lock(registry_mutex());
  auto& r = registry();
  auto it = std::find_if(r.begin(), r.end(), [&](const auto& f) { return f.name == factory.name; });
  if (it == r.end()) r.push_back(std::move(factory));
  else *it = std::move(factory);
}

const BackendFactory* find_backend(std::string_view name) {
  std::lock_guard lock(registry_mutex());
  auto& r = registry();
  auto it = std::find_if(r.begin(), r.end(), [&](const auto& f) { return f.name == name; });
  return it == r.end() ? nullptr : &*it;
}

std::vector<std::string> registered_backends() {
  std::lock_guard lock(registry_mutex());
  std::vector<std::string> out;
  for (const auto& f : registry()) out.push_back(f.name);
  return out;
}

}  // namespace lnfs::backend
