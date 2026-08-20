#include "server/rpcbind.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>

namespace lnfs::server {
namespace {

void put32(std::array<std::byte, 128>& buf, size_t& pos, uint32_t value) {
  value = htonl(value);
  std::memcpy(buf.data() + pos, &value, 4);
  pos += 4;
}

uint32_t get32(const std::byte* p) {
  uint32_t value;
  std::memcpy(&value, p, 4);
  return ntohl(value);
}

Result<void> call_portmapper(uint32_t procedure, uint32_t program, uint32_t version,
                             uint16_t port) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return Err(errno_from(errno));
  timeval timeout{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(111);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  uint32_t xid = static_cast<uint32_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  std::array<std::byte, 128> request{};
  size_t n = 0;
  put32(request, n, xid);
  put32(request, n, 0);       // CALL
  put32(request, n, 2);       // RPC version
  put32(request, n, 100000);  // portmapper
  put32(request, n, 2);       // PMAP v2
  put32(request, n, procedure);
  put32(request, n, 0);  // AUTH_NONE credential
  put32(request, n, 0);
  put32(request, n, 0);  // AUTH_NONE verifier
  put32(request, n, 0);
  put32(request, n, program);
  put32(request, n, version);
  put32(request, n, IPPROTO_TCP);
  put32(request, n, port);
  ssize_t sent = sendto(fd, request.data(), n, 0, reinterpret_cast<sockaddr*>(&address),
                        sizeof(address));
  if (sent != static_cast<ssize_t>(n)) {
    int e = errno;
    close(fd);
    return Err(errno_from(e ? e : EIO));
  }
  std::array<std::byte, 128> reply{};
  ssize_t got = recv(fd, reply.data(), reply.size(), 0);
  int e = errno;
  close(fd);
  // xid, REPLY, MSG_ACCEPTED, verf flavor/length, SUCCESS, bool result.
  if (got < 28) return Err(errno_from(got < 0 ? e : EPROTO));
  if (get32(reply.data()) != xid || get32(reply.data() + 4) != 1 ||
      get32(reply.data() + 8) != 0)
    return Err(errno_from(EPROTO));
  uint32_t verf_len = get32(reply.data() + 16);
  size_t accept_pos = 20 + ((verf_len + 3) & ~size_t(3));
  if (accept_pos + 8 > static_cast<size_t>(got) || get32(reply.data() + accept_pos) != 0 ||
      get32(reply.data() + accept_pos + 4) == 0)
    return Err(errno_from(EACCES));
  return {};
}

}  // namespace

Result<void> rpcbind_set(uint32_t program, uint32_t version, uint16_t port) {
  return call_portmapper(1, program, version, port);
}

Result<void> rpcbind_unset(uint32_t program, uint32_t version) {
  return call_portmapper(2, program, version, 0);
}

}  // namespace lnfs::server
