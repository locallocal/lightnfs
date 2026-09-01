#include "transport/tls.hpp"

#include "util/log.hpp"

#ifdef LNFS_TLS

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cstring>

#include "runtime/io.hpp"

namespace lnfs::transport {

using namespace lnfs::rt;

namespace {
std::string ssl_errs() {
  std::string out;
  unsigned long e;
  char buf[256];
  while ((e = ERR_get_error()) != 0) {
    ERR_error_string_n(e, buf, sizeof buf);
    if (!out.empty()) out += "; ";
    out += buf;
  }
  return out.empty() ? "unknown TLS error" : out;
}
}  // namespace

bool tls_available() { return true; }

Result<std::unique_ptr<TlsContext>> TlsContext::create(const TlsConfig& cfg) {
  if (cfg.cert.empty() || cfg.key.empty()) return Err(errno_from(EINVAL));
  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx) {
    LNFS_ERROR("TLS: SSL_CTX_new failed: {}", ssl_errs());
    return Err(errno_from(ENOMEM));
  }
  // RFC 9289 targets TLS 1.3 (Linux xprtsec=tls), but allow 1.2 for broader clients.
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                            SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  auto fail = [&](int e) {
    LNFS_ERROR("TLS: {}", ssl_errs());
    SSL_CTX_free(ctx);
    return Err(errno_from(e));
  };
  if (SSL_CTX_use_certificate_chain_file(ctx, cfg.cert.c_str()) != 1) return fail(EINVAL);
  if (SSL_CTX_use_PrivateKey_file(ctx, cfg.key.c_str(), SSL_FILETYPE_PEM) != 1)
    return fail(EINVAL);
  if (SSL_CTX_check_private_key(ctx) != 1) return fail(EINVAL);
  if (!cfg.ca.empty()) {
    if (SSL_CTX_load_verify_locations(ctx, cfg.ca.c_str(), nullptr) != 1)
      return fail(EINVAL);
    int mode = SSL_VERIFY_PEER;
    if (cfg.require_client_cert) mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
    SSL_CTX_set_verify(ctx, mode, nullptr);
  }
  return std::unique_ptr<TlsContext>(new TlsContext(ctx));
}

TlsContext::~TlsContext() {
  if (ctx_) SSL_CTX_free(static_cast<SSL_CTX*>(ctx_));
}

Result<std::unique_ptr<TlsConn>> TlsConn::create(const TlsContext& tctx) {
  SSL* ssl = SSL_new(static_cast<SSL_CTX*>(tctx.native()));
  if (!ssl) return Err(errno_from(ENOMEM));
  BIO* rbio = BIO_new(BIO_s_mem());
  BIO* wbio = BIO_new(BIO_s_mem());
  if (!rbio || !wbio) {
    if (rbio) BIO_free(rbio);
    if (wbio) BIO_free(wbio);
    SSL_free(ssl);
    return Err(errno_from(ENOMEM));
  }
  // An empty memory rbio must mean "want more bytes", not EOF, or SSL_read would treat
  // a momentarily-drained buffer as a closed connection.
  BIO_set_mem_eof_return(rbio, -1);
  SSL_set_bio(ssl, rbio, wbio);  // SSL takes ownership of both BIOs
  return std::unique_ptr<TlsConn>(new TlsConn(ssl));
}

TlsConn::~TlsConn() {
  if (ssl_) SSL_free(static_cast<SSL*>(ssl_));
}

rt::Task<Result<void>> TlsConn::flush_out(int fd) {
  auto lk = co_await send_mu_.lock();  // one ciphertext drain-and-send at a time
  SSL* ssl = static_cast<SSL*>(ssl_);
  BIO* wbio = SSL_get_wbio(ssl);
  for (;;) {
    int pending = BIO_read(wbio, netbuf_.data(), static_cast<int>(netbuf_.size()));
    if (pending <= 0) break;  // nothing more buffered
    size_t sent = 0;
    while (sent < static_cast<size_t>(pending)) {
      iovec iov{netbuf_.data() + sent, static_cast<size_t>(pending) - sent};
      int n = co_await uring_sendv(fd, &iov, 1);
      if (n == -EINTR || n == -EAGAIN) continue;
      if (n <= 0) co_return Err(n == 0 ? errno_from(EPIPE) : errno_from_neg(n));
      sent += static_cast<size_t>(n);
    }
  }
  co_return Result<void>{};
}

rt::Task<Result<void>> TlsConn::feed_in(int fd) {
  for (;;) {
    int n = co_await uring_recv(
        fd, std::span<std::byte>(netbuf_.data(), netbuf_.size()));
    if (n == -EINTR || n == -EAGAIN) continue;
    if (n == 0) co_return Err(Errno::kEof);
    if (n < 0) co_return Err(errno_from_neg(n));
    SSL* ssl = static_cast<SSL*>(ssl_);
    BIO* rbio = SSL_get_rbio(ssl);
    int off = 0;
    while (off < n) {
      int w = BIO_write(rbio, netbuf_.data() + off, n - off);
      if (w <= 0) co_return Err(errno_from(EPROTO));  // mem BIO never refuses a write
      off += w;
    }
    co_return Result<void>{};
  }
}

rt::Task<Result<void>> TlsConn::accept(int fd) {
  SSL* ssl = static_cast<SSL*>(ssl_);
  SSL_set_accept_state(ssl);
  for (;;) {
    int r = SSL_do_handshake(ssl);
    if (auto f = co_await flush_out(fd); !f) co_return Err(f.error());
    if (r == 1) co_return Result<void>{};  // handshake complete
    int err = SSL_get_error(ssl, r);
    if (err == SSL_ERROR_WANT_READ) {
      if (auto f = co_await feed_in(fd); !f) co_return Err(f.error());
    } else if (err != SSL_ERROR_WANT_WRITE) {
      LNFS_WARN("TLS handshake failed: {}", ssl_errs());
      co_return Err(errno_from(EPROTO));
    }
  }
}

rt::Task<Result<uint32_t>> TlsConn::read(int fd, std::span<std::byte> out) {
  SSL* ssl = static_cast<SSL*>(ssl_);
  for (;;) {
    int r = SSL_read(ssl, out.data(), static_cast<int>(out.size()));
    if (r > 0) {
      if (auto f = co_await flush_out(fd); !f) co_return Err(f.error());  // e.g. KeyUpdate
      co_return static_cast<uint32_t>(r);
    }
    int err = SSL_get_error(ssl, r);
    if (err == SSL_ERROR_ZERO_RETURN) co_return Err(Errno::kEof);  // clean TLS close
    if (auto f = co_await flush_out(fd); !f) co_return Err(f.error());
    if (err == SSL_ERROR_WANT_READ) {
      if (auto f = co_await feed_in(fd); !f) co_return Err(f.error());
    } else if (err != SSL_ERROR_WANT_WRITE) {
      co_return Err(errno_from(EPROTO));
    }
  }
}

rt::Task<Result<void>> TlsConn::write(int fd, std::span<const std::byte> in) {
  SSL* ssl = static_cast<SSL*>(ssl_);
  size_t off = 0;
  while (off < in.size()) {
    int r = SSL_write(ssl, in.data() + off, static_cast<int>(in.size() - off));
    if (r > 0) {
      off += static_cast<size_t>(r);
      if (auto f = co_await flush_out(fd); !f) co_return Err(f.error());
      continue;
    }
    int err = SSL_get_error(ssl, r);
    if (auto f = co_await flush_out(fd); !f) co_return Err(f.error());
    if (err == SSL_ERROR_WANT_READ) {
      if (auto f = co_await feed_in(fd); !f) co_return Err(f.error());
    } else if (err != SSL_ERROR_WANT_WRITE) {
      co_return Err(errno_from(EPROTO));
    }
  }
  co_return Result<void>{};
}

}  // namespace lnfs::transport

#else  // !LNFS_TLS — no OpenSSL at build time: honest stubs.

namespace lnfs::transport {

bool tls_available() { return false; }

Result<std::unique_ptr<TlsContext>> TlsContext::create(const TlsConfig&) {
  return Err(errno_from(EOPNOTSUPP));
}
TlsContext::~TlsContext() = default;

Result<std::unique_ptr<TlsConn>> TlsConn::create(const TlsContext&) {
  return Err(errno_from(EOPNOTSUPP));
}
TlsConn::~TlsConn() = default;
rt::Task<Result<void>> TlsConn::accept(int) { co_return Err(errno_from(EOPNOTSUPP)); }
rt::Task<Result<uint32_t>> TlsConn::read(int, std::span<std::byte>) {
  co_return Err(errno_from(EOPNOTSUPP));
}
rt::Task<Result<void>> TlsConn::write(int, std::span<const std::byte>) {
  co_return Err(errno_from(EOPNOTSUPP));
}

}  // namespace lnfs::transport

#endif
