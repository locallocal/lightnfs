#pragma once

#include <netinet/in.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "backend/api.hpp"
#include "rpc/auth.hpp"

namespace lnfs::core {

enum class Squash { kNone, kRoot, kAll };

class Cidr {
 public:
  static Result<Cidr> parse(std::string_view text);
  bool contains(const sockaddr_storage& peer) const;
  const std::string& text() const { return text_; }

 private:
  int family_ = AF_UNSPEC;
  std::array<uint8_t, 16> address_{};
  uint8_t prefix_ = 0;
  std::string text_;
};

struct ServerConfig {
  int reactors = 0;
  int offload_threads = 16;
  uint16_t port = 2049;
  uint16_t mount_port = 20048;
  bool rpcbind = true;
  bool builtin_portmap = false;
  std::string state_dir = "/var/lib/lightnfs";
  int max_connections = 4096;
  uint32_t max_request_size = (1u << 20) + (64u << 10);
  int inflight_per_conn = 64;
  int per_peer_limit = 128;
  uint64_t drc_ttl_ms = 120000;
  uint64_t drc_mem = 64u << 20;
  bool enable_v4 = true;
  uint32_t lease_seconds = 90;        // v4 lease (also the grace window)
  uint32_t courtesy_multiplier = 24;  // courtesy window = multiplier × lease
  std::string ctl_socket;   // default: <state_dir>/ctl.sock; "" resolves at startup
  uint16_t metrics_port = 0;  // 0 = disabled
  std::string log_level = "info";  // debug enables the per-request summary line (08 §8.2)
};

struct ExportConfig {
  std::string path;
  std::string backend = "local";
  uint32_t fsid = 0;
  std::vector<std::string> clients{"0.0.0.0/0", "::/0"};
  Squash squash = Squash::kRoot;
  uint32_t anon_uid = 65534;
  uint32_t anon_gid = 65534;
  bool readonly = false;
  backend::BackendConfig backend_config;
};

struct Config {
  ServerConfig server;
  std::vector<ExportConfig> exports;
};

Result<Config> parse_config(std::string_view toml);
Result<Config> load_config(const std::string& path);
Result<void> validate_config(const Config& config);

struct ExportEntry {
  std::string path;
  uint32_t fsid = 0;
  std::vector<Cidr> clients;
  Squash squash = Squash::kRoot;
  uint32_t anon_uid = 65534;
  uint32_t anon_gid = 65534;
  bool readonly = false;
  std::unique_ptr<backend::Backend> backend;
};

struct MappedCred {
  uint32_t uid = 65534;
  uint32_t gid = 65534;
  std::vector<uint32_t> groups;
  backend::Cred view() const { return {uid, gid, groups}; }
};

class ExportTable {
 public:
  static Result<std::unique_ptr<ExportTable>> build(Config config);
  Result<void> add(ExportConfig cfg, std::unique_ptr<backend::Backend> backend);

  ExportEntry* by_fsid(uint32_t fsid);
  const ExportEntry* by_fsid(uint32_t fsid) const;
  // Longest export path prefix, component-boundary checked.
  ExportEntry* for_mount_path(std::string_view path, std::string& relative);
  bool check_client(const sockaddr_storage& peer, const ExportEntry& entry) const;
  MappedCred squash_cred(const rpc::Cred& cred, const ExportEntry& entry) const;
  const std::vector<std::unique_ptr<ExportEntry>>& entries() const { return entries_; }

 private:
  std::vector<std::unique_ptr<ExportEntry>> entries_;
};

}  // namespace lnfs::core
