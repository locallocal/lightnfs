#pragma once
// RPC-over-TLS (RFC 9289): opportunistic transport encryption for ONC RPC, negotiated
// with an AUTH_TLS probe on the NULL procedure and a "STARTTLS" server reply, after
// which the same TCP connection carries a TLS session (design 09 §"长期观察项", plan
// doc 10 §5.4).  "TLS 保通道、AUTH_SYS 报身份": the channel is authenticated/encrypted
// by TLS while AUTH_SYS continues to declare the user identity.
//
// Async model: all socket I/O stays on io_uring (uring_recv/uring_sendv).  OpenSSL does
// crypto in a pair of memory BIOs; TlsConn shuttles ciphertext between those BIOs and
// the socket, so the reactor thread never blocks in an OpenSSL socket call.  Every
// SSL_* call is synchronous (it returns WANT_READ/WANT_WRITE); the coroutine only
// suspends between complete SSL_* calls, so a connection's single reader loop and its
// reply-sending handlers never re-enter OpenSSL concurrently (one reactor, one thread).
//
// The whole module compiles to honest stubs when the build has no OpenSSL (LNFS_TLS
// undefined): TlsContext::create fails with EOPNOTSUPP and config validation rejects a
// non-off tls mode with a clear "built without TLS" error.

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "runtime/buffer.hpp"
#include "runtime/sync.hpp"
#include "runtime/task.hpp"
#include "util/result.hpp"

namespace lnfs::transport {

// Server TLS enforcement (RFC 9289 is opportunistic by default):
//   kOff       never offer STARTTLS; a probe is declined and the connection stays clear.
//   kOptional  offer STARTTLS when probed; still serve cleartext clients.
//   kRequired  offer STARTTLS; refuse to serve NFS/MOUNT ops on a cleartext connection
//              (a non-NULL RPC that never upgraded is denied AUTH_TOOWEAK).
enum class TlsPolicy { kOff, kOptional, kRequired };

struct TlsConfig {
  TlsPolicy policy = TlsPolicy::kOff;
  std::string cert;  // server certificate chain, PEM
  std::string key;   // server private key, PEM
  std::string ca;    // optional CA bundle: when set, client certs are requested+verified
  bool require_client_cert = false;  // mutual TLS (needs `ca`)
};

// True when this build was compiled with OpenSSL support.
bool tls_available();

// Process-wide server TLS context (one SSL_CTX, the loaded cert/key).  Shared by every
// connection; thread-safe for creating per-connection sessions.
class TlsContext {
 public:
  static Result<std::unique_ptr<TlsContext>> create(const TlsConfig& cfg);
  ~TlsContext();
  TlsContext(const TlsContext&) = delete;
  TlsContext& operator=(const TlsContext&) = delete;

  void* native() const { return ctx_; }  // SSL_CTX* (opaque to non-TLS TUs)

 private:
  explicit TlsContext(void* ctx) : ctx_(ctx) {}
  void* ctx_ = nullptr;
};

// One TLS session over one connection's fd.  Owns the SSL object and its memory BIOs.
class TlsConn {
 public:
  static Result<std::unique_ptr<TlsConn>> create(const TlsContext& ctx);
  ~TlsConn();
  TlsConn(const TlsConn&) = delete;
  TlsConn& operator=(const TlsConn&) = delete;

  // Server-side handshake on `fd` (must run on the fd's home reactor).
  rt::Task<Result<void>> accept(int fd);
  // Reads up to out.size() plaintext bytes; 0 return maps to Err(kEof) at a clean close.
  rt::Task<Result<uint32_t>> read(int fd, std::span<std::byte> out);
  // Writes all of `in` as TLS application data.
  rt::Task<Result<void>> write(int fd, std::span<const std::byte> in);

  explicit TlsConn(void* ssl) : ssl_(ssl) {}

 private:
  // Drains OpenSSL's outgoing BIO to the socket; feeds one socket read into the
  // incoming BIO.  Both return Err on socket error / EOF.
  rt::Task<Result<void>> flush_out(int fd);
  rt::Task<Result<void>> feed_in(int fd);

  void* ssl_ = nullptr;  // SSL*
  std::vector<std::byte> netbuf_ = std::vector<std::byte>(16384);  // ciphertext scratch
  // Serializes ciphertext egress: SSL_write on a reply handler and an incidental write
  // from the read loop's SSL_read (TLS 1.3 KeyUpdate/ticket) both drain the outgoing
  // BIO, so their socket sends must not interleave and reorder the byte stream.
  rt::AsyncMutex send_mu_;
};

}  // namespace lnfs::transport
