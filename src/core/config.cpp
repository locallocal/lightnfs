#include "core/config.hpp"

#include <arpa/inet.h>
#include <sys/stat.h>

#include <charconv>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "backend/api.hpp"

namespace lnfs::core {
namespace {

std::string_view trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  return value;
}

std::string strip_comment(std::string_view line) {
  bool quoted = false;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) quoted = !quoted;
    if (line[i] == '#' && !quoted) return std::string(line.substr(0, i));
  }
  return std::string(line);
}

Result<std::string> string_value(std::string_view value) {
  value = trim(value);
  if (value.size() < 2 || value.front() != '"' || value.back() != '"')
    return Err(errno_from(EINVAL));
  std::string out;
  for (size_t i = 1; i + 1 < value.size(); ++i) {
    if (value[i] == '\\' && i + 2 < value.size()) {
      char next = value[++i];
      out.push_back(next == 'n' ? '\n' : next == 't' ? '\t' : next);
    } else {
      out.push_back(value[i]);
    }
  }
  return out;
}

Result<uint64_t> uint_value(std::string_view value) {
  value = trim(value);
  uint64_t out = 0;
  auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
  if (ec != std::errc{} || end != value.data() + value.size()) return Err(errno_from(EINVAL));
  return out;
}

Result<bool> bool_value(std::string_view value) {
  value = trim(value);
  if (value == "true") return true;
  if (value == "false") return false;
  return Err(errno_from(EINVAL));
}

Result<uint64_t> size_value(std::string_view value) {
  value = trim(value);
  if (!value.empty() && value.front() == '"') {
    auto parsed = string_value(value);
    if (!parsed) return Err(parsed.error());
    std::string_view s = *parsed;
    size_t digits = 0;
    while (digits < s.size() && std::isdigit(static_cast<unsigned char>(s[digits]))) ++digits;
    auto base = uint_value(s.substr(0, digits));
    if (!base) return Err(base.error());
    std::string_view suffix = s.substr(digits);
    uint64_t mul = suffix.empty() || suffix == "B"   ? 1
                   : suffix == "KiB"                 ? 1024
                   : suffix == "MiB"                 ? 1024 * 1024
                   : suffix == "GiB"                 ? 1024ull * 1024 * 1024
                                                       : 0;
    if (!mul || *base > UINT64_MAX / mul) return Err(errno_from(EINVAL));
    return *base * mul;
  }
  return uint_value(value);
}

Result<std::vector<std::string>> string_array(std::string_view value) {
  value = trim(value);
  if (value.size() < 2 || value.front() != '[' || value.back() != ']')
    return Err(errno_from(EINVAL));
  value = trim(value.substr(1, value.size() - 2));
  std::vector<std::string> out;
  while (!value.empty()) {
    if (value.front() != '"') return Err(errno_from(EINVAL));
    size_t end = 1;
    while (end < value.size() && (value[end] != '"' || value[end - 1] == '\\')) ++end;
    if (end == value.size()) return Err(errno_from(EINVAL));
    auto item = string_value(value.substr(0, end + 1));
    if (!item) return Err(item.error());
    out.push_back(std::move(*item));
    value = trim(value.substr(end + 1));
    if (value.empty()) break;
    if (value.front() != ',') return Err(errno_from(EINVAL));
    value = trim(value.substr(1));
  }
  return out;
}

std::string normalize_path(std::string path) {
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  return path;
}

}  // namespace

Result<Cidr> Cidr::parse(std::string_view value) {
  Cidr out;
  out.text_ = std::string(value);
  size_t slash = value.find('/');
  std::string host(value.substr(0, slash));
  uint64_t prefix = 0;
  if (inet_pton(AF_INET, host.c_str(), out.address_.data()) == 1) {
    out.family_ = AF_INET;
    prefix = slash == std::string_view::npos ? 32 : LNFS_TRY(uint_value(value.substr(slash + 1)));
    if (prefix > 32) return Err(errno_from(EINVAL));
  } else if (inet_pton(AF_INET6, host.c_str(), out.address_.data()) == 1) {
    out.family_ = AF_INET6;
    prefix = slash == std::string_view::npos ? 128 : LNFS_TRY(uint_value(value.substr(slash + 1)));
    if (prefix > 128) return Err(errno_from(EINVAL));
  } else {
    return Err(errno_from(EINVAL));
  }
  out.prefix_ = static_cast<uint8_t>(prefix);
  return out;
}

bool Cidr::contains(const sockaddr_storage& peer) const {
  const uint8_t* actual = nullptr;
  if (peer.ss_family == family_) {
    actual = family_ == AF_INET
                 ? reinterpret_cast<const uint8_t*>(
                       &reinterpret_cast<const sockaddr_in*>(&peer)->sin_addr)
                 : reinterpret_cast<const uint8_t*>(
                       &reinterpret_cast<const sockaddr_in6*>(&peer)->sin6_addr);
  } else if (family_ == AF_INET && peer.ss_family == AF_INET6) {
    const auto* addr6 = &reinterpret_cast<const sockaddr_in6*>(&peer)->sin6_addr;
    if (!IN6_IS_ADDR_V4MAPPED(addr6)) return false;
    actual = reinterpret_cast<const uint8_t*>(addr6) + 12;
  } else {
    return false;
  }
  size_t full = prefix_ / 8;
  uint8_t partial = prefix_ % 8;
  if (!std::equal(address_.begin(), address_.begin() + full, actual)) return false;
  if (!partial) return true;
  uint8_t mask = static_cast<uint8_t>(0xffu << (8 - partial));
  return (address_[full] & mask) == (actual[full] & mask);
}

Result<Config> parse_config(std::string_view text) {
  Config config;
  enum class Section { kNone, kServer, kLimits, kProtocol, kExport, kExportBackend };
  Section section = Section::kNone;
  ExportConfig* exp = nullptr;
  std::istringstream input{std::string(text)};
  std::string raw_line;
  while (std::getline(input, raw_line)) {
    std::string clean = strip_comment(raw_line);
    std::string_view line = trim(clean);
    if (line.empty()) continue;
    if (line == "[[export]]") {
      config.exports.emplace_back();
      exp = &config.exports.back();
      section = Section::kExport;
      continue;
    }
    if (line.front() == '[' && line.back() == ']') {
      if (line == "[server]") section = Section::kServer;
      else if (line == "[limits]") section = Section::kLimits;
      else if (line == "[protocol]") section = Section::kProtocol;
      else if (line.starts_with("[export.") && exp) section = Section::kExportBackend;
      else return Err(errno_from(EINVAL));
      continue;
    }
    size_t equal = line.find('=');
    if (equal == std::string_view::npos) return Err(errno_from(EINVAL));
    std::string key(trim(line.substr(0, equal)));
    std::string_view value = trim(line.substr(equal + 1));
    if (section == Section::kServer) {
      if (key == "reactors") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n > INT_MAX) return Err(errno_from(EINVAL));
        config.server.reactors = static_cast<int>(n);
      } else if (key == "offload_threads") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n > INT_MAX) return Err(errno_from(EINVAL));
        config.server.offload_threads = static_cast<int>(n);
      } else if (key == "port") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n > UINT16_MAX) return Err(errno_from(EINVAL));
        config.server.port = static_cast<uint16_t>(n);
      } else if (key == "mount_port") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n > UINT16_MAX) return Err(errno_from(EINVAL));
        config.server.mount_port = static_cast<uint16_t>(n);
      }
      else if (key == "rpcbind") config.server.rpcbind = LNFS_TRY(bool_value(value));
      else if (key == "builtin_portmap") config.server.builtin_portmap = LNFS_TRY(bool_value(value));
      else if (key == "state_dir") config.server.state_dir = LNFS_TRY(string_value(value));
      else if (key == "max_connections") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n > INT_MAX) return Err(errno_from(EINVAL));
        config.server.max_connections = static_cast<int>(n);
      } else if (key == "per_peer_limit") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n == 0 || n > 1u << 20) return Err(errno_from(EINVAL));
        config.server.per_peer_limit = static_cast<int>(n);
      } else if (key == "ctl_socket") {
        config.server.ctl_socket = LNFS_TRY(string_value(value));
      } else if (key == "metrics_port") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n > 65535) return Err(errno_from(EINVAL));
        config.server.metrics_port = static_cast<uint16_t>(n);
      } else if (key == "metrics_bind") {
        config.server.metrics_bind = LNFS_TRY(string_value(value));
      } else if (key == "metrics_allow") {
        config.server.metrics_allow = LNFS_TRY(string_array(value));
      } else if (key == "server_owner") {
        config.server.server_owner = LNFS_TRY(string_value(value));
      } else if (key == "server_scope") {
        config.server.server_scope = LNFS_TRY(string_value(value));
      } else if (key == "max_request_size") {
        uint64_t n = LNFS_TRY(size_value(value));
        if (n > UINT32_MAX) return Err(errno_from(EINVAL));
        config.server.max_request_size = static_cast<uint32_t>(n);
      } else if (key == "log_level") {
        auto lv = LNFS_TRY(string_value(value));
        if (lv != "debug" && lv != "info" && lv != "warn" && lv != "error")
          return Err(errno_from(EINVAL));
        config.server.log_level = lv;
      }
    } else if (section == Section::kLimits) {
      if (key == "inflight_per_conn") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n > INT_MAX) return Err(errno_from(EINVAL));
        config.server.inflight_per_conn = static_cast<int>(n);
      }
      // rtmax/wtmax/dtpref are backend limits in phase 1; unknown limit keys are accepted.
    } else if (section == Section::kProtocol) {
      if (key == "v3") LNFS_TRY(bool_value(value));
      else if (key == "v4") config.server.enable_v4 = LNFS_TRY(bool_value(value));
      else if (key == "lease" || key == "grace") {
        // Seconds, with optional "s" suffix ("90s").  grace == lease by design (07 §7.5).
        std::string s = LNFS_TRY(string_value(value));
        uint64_t n = 0;
        size_t digits = 0;
        while (digits < s.size() && std::isdigit(static_cast<unsigned char>(s[digits])))
          n = n * 10 + static_cast<uint64_t>(s[digits++] - '0');
        std::string_view suffix = std::string_view(s).substr(digits);
        if (digits == 0 || !(suffix.empty() || suffix == "s") || n == 0 || n > 3600)
          return Err(errno_from(EINVAL));
        config.server.lease_seconds = static_cast<uint32_t>(n);
      } else if (key == "courtesy_multiplier") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n == 0 || n > 1000) return Err(errno_from(EINVAL));
        config.server.courtesy_multiplier = static_cast<uint32_t>(n);
      }
      else if (key == "drc_ttl") {
        // Duration string, seconds ("120s") or milliseconds ("500ms").
        std::string s = LNFS_TRY(string_value(value));
        uint64_t n = 0;
        size_t digits = 0;
        while (digits < s.size() && std::isdigit(static_cast<unsigned char>(s[digits])))
          n = n * 10 + static_cast<uint64_t>(s[digits++] - '0');
        std::string_view suffix = std::string_view(s).substr(digits);
        if (digits == 0) return Err(errno_from(EINVAL));
        if (suffix == "s" || suffix.empty()) config.server.drc_ttl_ms = n * 1000;
        else if (suffix == "ms") config.server.drc_ttl_ms = n;
        else return Err(errno_from(EINVAL));
      } else if (key == "drc_mem")
        config.server.drc_mem = LNFS_TRY(size_value(value));
      else
        return Err(errno_from(EINVAL));
    } else if (section == Section::kExport && exp) {
      if (key == "path") exp->path = normalize_path(LNFS_TRY(string_value(value)));
      else if (key == "backend") exp->backend = LNFS_TRY(string_value(value));
      else if (key == "fsid") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n > UINT32_MAX) return Err(errno_from(EINVAL));
        exp->fsid = static_cast<uint32_t>(n);
      }
      else if (key == "clients") exp->clients = LNFS_TRY(string_array(value));
      else if (key == "readonly") exp->readonly = LNFS_TRY(bool_value(value));
      else if (key == "anon_uid") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n > UINT32_MAX) return Err(errno_from(EINVAL));
        exp->anon_uid = static_cast<uint32_t>(n);
      } else if (key == "anon_gid") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n > UINT32_MAX) return Err(errno_from(EINVAL));
        exp->anon_gid = static_cast<uint32_t>(n);
      }
      else if (key == "squash") {
        std::string s = LNFS_TRY(string_value(value));
        if (s == "none") exp->squash = Squash::kNone;
        else if (s == "root") exp->squash = Squash::kRoot;
        else if (s == "all") exp->squash = Squash::kAll;
        else return Err(errno_from(EINVAL));
      }
    } else if (section == Section::kExportBackend && exp) {
      if (!value.empty() && value.front() == '"') exp->backend_config.values[key] = LNFS_TRY(string_value(value));
      else if (value == "true" || value == "false") exp->backend_config.values[key] = value;
      else exp->backend_config.values[key] = std::to_string(LNFS_TRY(uint_value(value)));
    } else {
      return Err(errno_from(EINVAL));
    }
  }
  return config;
}

Result<Config> load_config(const std::string& path) {
  std::ifstream input(path);
  if (!input) return Err(errno_from(errno ? errno : ENOENT));
  std::ostringstream contents;
  contents << input.rdbuf();
  return parse_config(contents.str());
}

Result<void> validate_config(const Config& config) {
  backend::register_builtin_backends();
  if (config.exports.empty() || config.server.offload_threads <= 0 ||
      config.server.max_connections <= 0 || config.server.inflight_per_conn <= 0)
    return Err(errno_from(EINVAL));
  for (const auto& cidr : config.server.metrics_allow)
    if (!Cidr::parse(cidr)) return Err(errno_from(EINVAL));
  std::set<uint32_t> fsids;
  std::set<std::string> paths;
  for (const auto& exp : config.exports) {
    if (exp.fsid == 0 || exp.path.empty() || !fsids.insert(exp.fsid).second ||
        !paths.insert(exp.path).second)
      return Err(errno_from(EINVAL));
    struct stat st {};
    if (stat(exp.path.c_str(), &st) < 0) return Err(errno_from(errno));
    if (!S_ISDIR(st.st_mode)) return Err(errno_from(ENOTDIR));
    if (!backend::find_backend(exp.backend)) return Err(errno_from(ENODEV));
    if (exp.clients.empty()) return Err(errno_from(EINVAL));
    for (const auto& client : exp.clients)
      if (!Cidr::parse(client)) return Err(errno_from(EINVAL));
  }
  return {};
}

Result<std::unique_ptr<ExportTable>> ExportTable::build(Config config) {
  LNFS_TRY(validate_config(config));
  auto table = std::make_unique<ExportTable>();
  for (auto& cfg : config.exports) {
    cfg.backend_config.path = cfg.path;
    cfg.backend_config.fsid = cfg.fsid;
    const auto* factory = backend::find_backend(cfg.backend);
    if (!factory) return Err(errno_from(ENODEV));
    auto made = factory->make(cfg.backend_config);
    if (!made) return Err(errno_from(EINVAL));
    LNFS_TRY(table->add(std::move(cfg), std::move(made)));
  }
  return table;
}

Result<void> ExportTable::add(ExportConfig cfg, std::unique_ptr<backend::Backend> backend) {
  if (!backend || cfg.fsid == 0 || by_fsid(cfg.fsid)) return Err(errno_from(EINVAL));
  auto entry = std::make_unique<ExportEntry>();
  entry->path = normalize_path(std::move(cfg.path));
  entry->fsid = cfg.fsid;
  entry->squash = cfg.squash;
  entry->anon_uid = cfg.anon_uid;
  entry->anon_gid = cfg.anon_gid;
  entry->readonly = cfg.readonly;
  entry->backend = std::move(backend);
  for (const auto& client : cfg.clients) entry->clients.push_back(LNFS_TRY(Cidr::parse(client)));
  entries_.push_back(std::move(entry));
  return {};
}

ExportEntry* ExportTable::by_fsid(uint32_t fsid) {
  for (auto& entry : entries_)
    if (entry->fsid == fsid) return entry.get();
  return nullptr;
}
const ExportEntry* ExportTable::by_fsid(uint32_t fsid) const {
  for (const auto& entry : entries_)
    if (entry->fsid == fsid) return entry.get();
  return nullptr;
}

ExportEntry* ExportTable::for_mount_path(std::string_view raw, std::string& relative) {
  std::string path = normalize_path(std::string(raw));
  ExportEntry* best = nullptr;
  for (auto& entry : entries_) {
    if (!path.starts_with(entry->path)) continue;
    if (path.size() != entry->path.size() &&
        !(entry->path == "/" || path[entry->path.size()] == '/'))
      continue;
    if (!best || entry->path.size() > best->path.size()) best = entry.get();
  }
  if (!best) return nullptr;
  relative = path.substr(best->path.size());
  while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
  return best;
}

bool ExportTable::check_client(const sockaddr_storage& peer, const ExportEntry& entry) const {
  return std::any_of(entry.clients.begin(), entry.clients.end(),
                     [&](const Cidr& cidr) { return cidr.contains(peer); });
}

MappedCred ExportTable::squash_cred(const rpc::Cred& cred, const ExportEntry& entry) const {
  MappedCred out{cred.uid, cred.gid, {cred.gids.begin(), cred.gids.end()}};
  if (entry.squash == Squash::kAll ||
      (entry.squash == Squash::kRoot && out.uid == 0)) {
    out.uid = entry.anon_uid;
    out.gid = entry.anon_gid;
    out.groups.clear();
  }
  return out;
}

}  // namespace lnfs::core
