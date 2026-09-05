#include "core/config.hpp"

#include <arpa/inet.h>
#include <sys/stat.h>
#include <unistd.h>

#include <charconv>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <sstream>

#include "backend/api.hpp"
#include "util/sha256.hpp"

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

// Duration string in milliseconds: "3s", "1500ms", or a bare number of seconds.
Result<uint64_t> duration_ms_value(std::string_view value) {
  std::string s = LNFS_TRY(string_value(value));
  uint64_t n = 0;
  size_t digits = 0;
  while (digits < s.size() && std::isdigit(static_cast<unsigned char>(s[digits])))
    n = n * 10 + static_cast<uint64_t>(s[digits++] - '0');
  std::string_view suffix = std::string_view(s).substr(digits);
  if (digits == 0) return Err(errno_from(EINVAL));
  if (suffix == "s" || suffix.empty()) return n * 1000;
  if (suffix == "ms") return n;
  return Err(errno_from(EINVAL));
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
  enum class Section {
    kNone, kServer, kLimits, kProtocol, kTls, kCluster, kExport, kExportBackend
  };
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
      else if (line == "[tls]") section = Section::kTls;
      else if (line == "[cluster]") section = Section::kCluster;
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
      } else if (key == "offload_heavy_threads") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n > INT_MAX) return Err(errno_from(EINVAL));
        config.server.offload_heavy_threads = static_cast<int>(n);
      } else if (key == "offload_queue_cap") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n == 0 || n > UINT32_MAX) return Err(errno_from(EINVAL));
        config.server.offload_queue_cap = static_cast<uint32_t>(n);
      } else if (key == "ring") {
        auto r = LNFS_TRY(string_value(value));
        if (r != "auto" && r != "uring" && r != "epoll") return Err(errno_from(EINVAL));
        config.server.ring = r;
      } else if (key == "ring_sqpoll") {
        config.server.ring_sqpoll = LNFS_TRY(bool_value(value));
      } else if (key == "bind") {
        config.server.bind = LNFS_TRY(string_value(value));
      } else if (key == "state_shards") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n == 0 || n > 4096) return Err(errno_from(EINVAL));
        config.server.state_shards = static_cast<uint32_t>(n);
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
      } else if (key == "slow_request_ms") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n > 3600000) return Err(errno_from(EINVAL));  // 0 disables the slow log
        config.server.slow_request_ms = static_cast<uint32_t>(n);
      } else if (key == "error_ring") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n == 0 || n > 65536) return Err(errno_from(EINVAL));
        config.server.error_ring = static_cast<uint32_t>(n);
      } else if (key == "log_file") {
        config.server.log_file = LNFS_TRY(string_value(value));
      } else if (key == "log_rotate_size") {
        uint64_t n = LNFS_TRY(size_value(value));
        if (n < (1u << 16)) return Err(errno_from(EINVAL));  // < 64K rotates constantly
        config.server.log_rotate_size = n;
      } else if (key == "log_rotate_keep") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n == 0 || n > 1000) return Err(errno_from(EINVAL));
        config.server.log_rotate_keep = static_cast<uint32_t>(n);
      }
    } else if (section == Section::kLimits) {
      if (key == "inflight_per_conn") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n > INT_MAX) return Err(errno_from(EINVAL));
        config.server.inflight_per_conn = static_cast<int>(n);
      } else if (key == "client_read_bps") {
        config.server.client_read_bps = LNFS_TRY(size_value(value));
      } else if (key == "client_write_bps") {
        config.server.client_write_bps = LNFS_TRY(size_value(value));
      } else if (key == "client_iops") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n > UINT32_MAX) return Err(errno_from(EINVAL));
        config.server.client_iops = static_cast<uint32_t>(n);
      }
      // rtmax/wtmax/dtpref are backend limits in phase 1; unknown limit keys are accepted.
    } else if (section == Section::kTls) {
      if (key == "mode") config.server.tls_mode = LNFS_TRY(string_value(value));
      else if (key == "cert") config.server.tls_cert = LNFS_TRY(string_value(value));
      else if (key == "key") config.server.tls_key = LNFS_TRY(string_value(value));
      else if (key == "ca") config.server.tls_ca = LNFS_TRY(string_value(value));
      else if (key == "client_cert")
        config.server.tls_require_client_cert = LNFS_TRY(bool_value(value));
      else return Err(errno_from(EINVAL));
    } else if (section == Section::kCluster) {
      auto& c = config.cluster;
      if (key == "enabled") c.enabled = LNFS_TRY(bool_value(value));
      else if (key == "id") c.id = LNFS_TRY(string_value(value));
      else if (key == "shared_dir")
        c.shared_dir = normalize_path(LNFS_TRY(string_value(value)));
      else if (key == "node") c.node = LNFS_TRY(string_value(value));
      else if (key == "role") c.role = LNFS_TRY(string_value(value));
      else if (key == "takeover") c.takeover = LNFS_TRY(string_value(value));
      else if (key == "takeover_hook") c.takeover_hook = LNFS_TRY(string_value(value));
      else if (key == "fence_lease") {
        uint64_t ms = LNFS_TRY(duration_ms_value(value));
        if (ms < 500 || ms > 60000) return Err(errno_from(EINVAL));
        c.fence_lease_ms = static_cast<uint32_t>(ms);
      } else if (key == "unsafe_skip_backend_checks")
        c.unsafe_skip_backend_checks = LNFS_TRY(bool_value(value));
      else return Err(errno_from(EINVAL));
    } else if (section == Section::kProtocol) {
      if (key == "v3") LNFS_TRY(bool_value(value));
      else if (key == "v4") config.server.enable_v4 = LNFS_TRY(bool_value(value));
      else if (key == "delegations") config.server.delegations = LNFS_TRY(bool_value(value));
      else if (key == "lease" || key == "grace") {
        // Seconds, with optional "s" suffix ("90s").  grace defaults to the lease but
        // is decoupled (plan doc 10 §4.4): "auto" (or omission) = lease; operators
        // often want grace < lease for faster restart recovery.
        std::string s = LNFS_TRY(string_value(value));
        if (key == "grace" && s == "auto") {
          config.server.grace_seconds = 0;
          continue;
        }
        uint64_t n = 0;
        size_t digits = 0;
        while (digits < s.size() && std::isdigit(static_cast<unsigned char>(s[digits])))
          n = n * 10 + static_cast<uint64_t>(s[digits++] - '0');
        std::string_view suffix = std::string_view(s).substr(digits);
        if (digits == 0 || !(suffix.empty() || suffix == "s") || n == 0 || n > 3600)
          return Err(errno_from(EINVAL));
        if (key == "lease") config.server.lease_seconds = static_cast<uint32_t>(n);
        else config.server.grace_seconds = static_cast<uint32_t>(n);
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
      else if (key == "read_bps") exp->read_bps = LNFS_TRY(size_value(value));
      else if (key == "write_bps") exp->write_bps = LNFS_TRY(size_value(value));
      else if (key == "iops") {
        uint64_t n = LNFS_TRY(uint_value(value));
        if (n > UINT32_MAX) return Err(errno_from(EINVAL));
        exp->iops = static_cast<uint32_t>(n);
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
  if (!config.server.bind.empty()) {  // listener bind must be an address literal
    in6_addr a6;
    in_addr a4;
    if (inet_pton(AF_INET, config.server.bind.c_str(), &a4) != 1 &&
        inet_pton(AF_INET6, config.server.bind.c_str(), &a6) != 1)
      return Err(errno_from(EINVAL));
  }
  {  // RPC-over-TLS (RFC 9289): validate the [tls] section up front (plan doc 10 §5.4).
    const auto& s = config.server;
    if (s.tls_mode != "off" && s.tls_mode != "optional" && s.tls_mode != "required")
      return Err(errno_from(EINVAL));
    if (s.tls_mode != "off") {
#ifndef LNFS_TLS
      return Err(errno_from(ENOTSUP));  // built without OpenSSL: a non-off mode is invalid
#endif
      if (s.tls_cert.empty() || s.tls_key.empty()) return Err(errno_from(EINVAL));
      struct stat st {};
      if (stat(s.tls_cert.c_str(), &st) < 0 || stat(s.tls_key.c_str(), &st) < 0)
        return Err(errno_from(errno ? errno : ENOENT));
      if (!s.tls_ca.empty() && stat(s.tls_ca.c_str(), &st) < 0)
        return Err(errno_from(errno ? errno : ENOENT));
      if (s.tls_require_client_cert && s.tls_ca.empty())
        return Err(errno_from(EINVAL));  // mutual TLS needs a CA bundle to verify against
    }
  }
  {  // Multi-gateway failover (design 09 §9.3, plan 10 A1): the [cluster] section.
    const auto& c = config.cluster;
    auto valid_role = c.role == "active" || c.role == "standby" || c.role == "auto";
    auto valid_takeover = c.takeover == "auto" || c.takeover == "manual";
    if (c.enabled) {
      if (!valid_role || !valid_takeover) return Err(errno_from(EINVAL));
      if (c.id.size() < 8 || c.id.size() > 64 ||
          !std::all_of(c.id.begin(), c.id.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '_' || ch == '-';
          }))
        return Err(errno_from(EINVAL));
      if (c.shared_dir.empty() || c.shared_dir.front() != '/')
        return Err(errno_from(EINVAL));
      if (!c.node.empty() && (c.node.size() > 64 || c.node.find('/') != std::string::npos))
        return Err(errno_from(EINVAL));
      // Identity is derived from `id` so every gateway presents the same server_owner/
      // scope; an explicit value would fork it (design 09 §9.3).
      if (!config.server.server_owner.empty() || !config.server.server_scope.empty())
        return Err(errno_from(EINVAL));
      if (!c.takeover_hook.empty()) {
        struct stat st {};
        if (stat(c.takeover_hook.c_str(), &st) < 0)
          return Err(errno_from(errno ? errno : ENOENT));
        if (!S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) return Err(errno_from(EACCES));
      }
    }
  }
  std::set<uint32_t> fsids;
  std::set<std::string> paths;
  for (const auto& exp : config.exports) {
    if (exp.fsid == 0 || exp.path.empty() || !fsids.insert(exp.fsid).second ||
        !paths.insert(exp.path).second)
      return Err(errno_from(EINVAL));
    const auto* factory = backend::find_backend(exp.backend);
    if (!factory) return Err(errno_from(ENODEV));
    if (!factory->virtual_path) {  // cluster backends: the path is a mount name only
      struct stat st {};
      if (stat(exp.path.c_str(), &st) < 0) return Err(errno_from(errno));
      if (!S_ISDIR(st.st_mode)) return Err(errno_from(ENOTDIR));
    }
    if (exp.clients.empty()) return Err(errno_from(EINVAL));
    for (const auto& client : exp.clients)
      if (!Cidr::parse(client)) return Err(errno_from(EINVAL));
  }
  return {};
}

std::string canonical_exports_text(const Config& config) {
  std::vector<const ExportConfig*> sorted;
  for (const auto& exp : config.exports) sorted.push_back(&exp);
  std::sort(sorted.begin(), sorted.end(), [](const ExportConfig* a, const ExportConfig* b) {
    return a->fsid != b->fsid ? a->fsid < b->fsid : a->path < b->path;
  });
  std::string out;
  for (const ExportConfig* exp : sorted) {
    const char* squash = exp->squash == Squash::kNone ? "none"
                         : exp->squash == Squash::kAll ? "all"
                                                       : "root";
    out += std::format("export path={} fsid={} backend={} readonly={} squash={} anon_uid={} "
                       "anon_gid={}\n",
                       exp->path, exp->fsid, exp->backend, exp->readonly ? 1 : 0, squash,
                       exp->anon_uid, exp->anon_gid);
    std::vector<std::pair<std::string, std::string>> keys(exp->backend_config.values.begin(),
                                                          exp->backend_config.values.end());
    std::sort(keys.begin(), keys.end());
    for (const auto& [key, value] : keys) {
      bool per_node = false;
      for (std::string_view exempt : kPerNodeBackendKeys)
        if (key == exempt) per_node = true;
      if (!per_node) out += std::format("  {}={}\n", key, value);
    }
  }
  return out;
}

std::string canonical_exports_digest(const Config& config) {
  return "sha256:" + util::sha256_hex(canonical_exports_text(config));
}

std::string cluster_node_name(const ClusterConfig& cluster) {
  if (!cluster.node.empty()) return cluster.node;
  char host[256] = "lightnfs";
  (void)::gethostname(host, sizeof host - 1);
  return host;
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
  std::vector<Cidr> clients;
  for (const auto& client : cfg.clients) clients.push_back(LNFS_TRY(Cidr::parse(client)));
  entry->set_clients(std::move(clients));
  entry->qos.read_bytes.configure(cfg.read_bps);
  entry->qos.write_bytes.configure(cfg.write_bps);
  entry->qos.ops.configure(cfg.iops);
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
  const auto& clients = entry.client_list();
  return std::any_of(clients.begin(), clients.end(),
                     [&](const Cidr& cidr) { return cidr.contains(peer); });
}

std::string ExportTable::reload_dynamic(const Config& fresh) {
  std::string report;
  std::set<uint32_t> seen;
  for (const auto& cfg : fresh.exports) {
    seen.insert(cfg.fsid);
    ExportEntry* entry = by_fsid(cfg.fsid);
    if (!entry) {
      report += std::format("export fsid={} ({}): new export, restart required\n",
                            cfg.fsid, cfg.path);
      continue;
    }
    if (normalize_path(cfg.path) != entry->path || cfg.squash != entry->squash ||
        cfg.readonly != entry->readonly || cfg.anon_uid != entry->anon_uid ||
        cfg.anon_gid != entry->anon_gid) {
      report += std::format(
          "export fsid={}: path/squash/readonly/anon changed, restart required "
          "(clients/qos still applied)\n",
          cfg.fsid);
    }
    std::vector<Cidr> clients;
    for (const auto& client : cfg.clients)
      clients.push_back(*Cidr::parse(client));  // fresh passed validate_config
    entry->set_clients(std::move(clients));
    entry->qos.read_bytes.configure(cfg.read_bps);
    entry->qos.write_bytes.configure(cfg.write_bps);
    entry->qos.ops.configure(cfg.iops);
    report += std::format("export fsid={}: clients ({}) and qos applied\n", cfg.fsid,
                          cfg.clients.size());
  }
  for (const auto& entry : entries_)
    if (!seen.contains(entry->fsid))
      report += std::format("export fsid={} ({}): removed from config, restart required "
                            "(still being served)\n",
                            entry->fsid, entry->path);
  return report;
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
