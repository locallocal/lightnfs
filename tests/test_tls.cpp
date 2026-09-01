// RPC-over-TLS (RFC 9289) end to end (design 09 §"长期观察项", plan doc 10 §5.4).
// A real OpenSSL client drives the STARTTLS negotiation and the TLS session against a
// live Runtime + Listener + Dispatcher serving a NULL/echo test program:
//   - the AUTH_TLS NULL probe earns a "STARTTLS" reply, the handshake completes, and an
//     echo RPC round-trips inside TLS;
//   - "optional" still serves a cleartext client;
//   - "required" refuses a cleartext NFS op with AUTH_TOOWEAK;
//   - "off" declines the probe (AUTH_NONE verifier) and the connection stays cleartext.
// Config parse/validate coverage for the [tls] section rounds it out.

#include "mini_test.hpp"

#include "core/config.hpp"

#ifdef LNFS_TLS

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "transport/listener.hpp"
#include "xdr/xdr.hpp"

using namespace lnfs;
using namespace lnfs::rt;
using namespace lnfs::rpc;
using namespace lnfs::transport;

namespace {

constexpr uint32_t kProg = 200077;

// A NULL + echo(proc 1) test program, mirroring test_transport_loopback.
void add_test_program(Dispatcher& disp) {
  disp.add({kProg, 1, 1, nullptr,
            [](void*, ConnCtx& c, RpcCall& call, const Cred&) -> Task<void> {
              xdr::XdrEnc enc(c.pool);
              encode_reply_success(enc, call.xid);
              if (call.proc == 1) {
                auto arg = call.args.opaque(1 << 16);
                if (!arg) {
                  co_await Dispatcher::reply_garbage_args(c, call.xid);
                  co_return;
                }
                enc.opaque(*arg);
              }
              co_await c.send(enc.take());
            }});
}

// ---- self-signed cert/key to temp files (OpenSSL 3) --------------------------------
struct TempCert {
  std::string dir, cert, key;
  bool ok = false;
  TempCert() {
    char tmpl[] = "/tmp/lnfs-tls-XXXXXX";
    dir = mkdtemp(tmpl);
    cert = dir + "/cert.pem";
    key = dir + "/key.pem";
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    if (!pkey) return;
    X509* x = X509_new();
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, pkey);
    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("lightnfs-test"), -1,
                               -1, 0);
    X509_set_issuer_name(x, name);
    ok = X509_sign(x, pkey, EVP_sha256()) != 0;
    if (ok) {
      FILE* fc = std::fopen(cert.c_str(), "wb");
      FILE* fk = std::fopen(key.c_str(), "wb");
      ok = fc && fk && PEM_write_X509(fc, x) &&
           PEM_write_PrivateKey(fk, pkey, nullptr, nullptr, 0, nullptr, nullptr);
      if (fc) std::fclose(fc);
      if (fk) std::fclose(fk);
    }
    X509_free(x);
    EVP_PKEY_free(pkey);
  }
  ~TempCert() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

int connect_loopback(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// One RPC call record: xid/CALL/vers/prog/vers/proc, cred+verf, optional echo payload.
std::vector<std::byte> build_call(uint32_t xid, uint32_t proc, uint32_t cred_flavor,
                                  std::string_view payload) {
  BufferPool pool;
  xdr::XdrEnc enc(pool);
  enc.u32(xid);
  enc.u32(0);  // CALL
  enc.u32(2);
  enc.u32(kProg);
  enc.u32(1);
  enc.u32(proc);
  enc.u32(cred_flavor);
  enc.u32(0);  // cred body len
  enc.u32(0);  // verf AUTH_NONE
  enc.u32(0);
  if (proc == 1)
    enc.opaque(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size()));
  auto body = enc.take().to_bytes();
  std::vector<std::byte> rec(4 + body.size());
  uint32_t hdr = xdr::to_be32(0x80000000u | static_cast<uint32_t>(body.size()));
  std::memcpy(rec.data(), &hdr, 4);
  std::memcpy(rec.data() + 4, body.data(), body.size());
  return rec;
}

// ---- byte transport over plain fd or SSL -------------------------------------------
bool raw_read(int fd, std::byte* p, size_t n) {
  while (n) {
    ssize_t r = read(fd, p, n);
    if (r <= 0) return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}
bool raw_write(int fd, const std::byte* p, size_t n) {
  while (n) {
    ssize_t r = write(fd, p, n);
    if (r <= 0) return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}
bool ssl_read_all(SSL* s, std::byte* p, size_t n) {
  while (n) {
    int r = SSL_read(s, p, static_cast<int>(n));
    if (r <= 0) return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}
bool ssl_write_all(SSL* s, const std::byte* p, size_t n) {
  while (n) {
    int r = SSL_write(s, p, static_cast<int>(n));
    if (r <= 0) return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

// Reads one record body (past the 4-byte marker) from a reader callback.
template <class Reader>
bool read_record(Reader&& rd, std::vector<std::byte>& out) {
  std::byte hdr[4];
  if (!rd(hdr, 4)) return false;
  uint32_t h;
  std::memcpy(&h, hdr, 4);
  h = xdr::from_be32(h);
  out.resize(h & 0x7fffffffu);
  return rd(out.data(), out.size());
}

// The server harness with a configurable TLS policy.
struct Server {
  Runtime rt{Runtime::Config{.reactors = 2, .offload_threads = 2}};
  Dispatcher disp;
  std::unique_ptr<TlsContext> tls;
  std::unique_ptr<Listener> listener;

  Server(TlsPolicy policy, const TempCert& cert) {
    add_test_program(disp);
    TransportConfig cfg;
    if (policy != TlsPolicy::kOff) {
      TlsConfig tc;
      tc.policy = policy;
      tc.cert = cert.cert;
      tc.key = cert.key;
      auto ctx = TlsContext::create(tc);
      if (ctx) {
        tls = std::move(*ctx);
        cfg.tls = tls.get();
        cfg.tls_policy = policy;
      }
    }
    auto l = Listener::create(0, cfg, disp, rt);
    listener = l ? std::move(*l) : nullptr;
    if (listener) listener->start();
    rt.start();
  }
  ~Server() {
    if (listener) listener->request_stop();
    rt.stop_and_join();
  }
  uint16_t port() { return listener ? listener->port() : 0; }
};

// Sends the AUTH_TLS NULL probe, returns true if the reply carried the STARTTLS
// verifier (flavor 7 + "STARTTLS").
bool do_starttls_probe(int fd) {
  auto probe = build_call(1, 0, kAuthTls, "");
  if (!raw_write(fd, probe.data(), probe.size())) return false;
  std::vector<std::byte> rep;
  if (!read_record([&](std::byte* p, size_t n) { return raw_read(fd, p, n); }, rep))
    return false;
  xdr::XdrDec dec{std::span<const std::byte>(rep.data(), rep.size())};
  auto xid = dec.u32();
  auto reply = dec.u32();
  auto accepted = dec.u32();
  auto verf_flavor = dec.u32();
  if (!xid || !reply || !accepted || !verf_flavor) return false;
  if (*reply != 1 || *accepted != 0) return false;
  if (*verf_flavor != kAuthTls) return false;  // declined: AUTH_NONE
  auto body = dec.opaque(64);
  return body && std::string(reinterpret_cast<const char*>(body->data()), body->size()) ==
                     "STARTTLS";
}

SSL_CTX* client_ctx() {
  SSL_CTX* c = SSL_CTX_new(TLS_client_method());
  SSL_CTX_set_verify(c, SSL_VERIFY_NONE, nullptr);  // test cert is self-signed
  return c;
}

}  // namespace

TEST(Tls, StartTlsHandshakeAndEcho) {
  TempCert cert;
  ASSERT_TRUE(cert.ok);
  Server srv(TlsPolicy::kOptional, cert);
  ASSERT_TRUE(srv.port() != 0);
  int fd = connect_loopback(srv.port());
  ASSERT_TRUE(fd >= 0);

  ASSERT_TRUE(do_starttls_probe(fd));  // RFC 9289 STARTTLS reply

  SSL_CTX* cctx = client_ctx();
  SSL* ssl = SSL_new(cctx);
  SSL_set_fd(ssl, fd);
  ASSERT_TRUE(SSL_connect(ssl) == 1);  // TLS handshake over the same connection

  // An echo RPC now travels inside TLS.
  auto call = build_call(42, 1, 0, "over-tls");
  ASSERT_TRUE(ssl_write_all(ssl, call.data(), call.size()));
  std::vector<std::byte> rep;
  ASSERT_TRUE(read_record([&](std::byte* p, size_t n) { return ssl_read_all(ssl, p, n); },
                          rep));
  xdr::XdrDec dec{std::span<const std::byte>(rep.data(), rep.size())};
  EXPECT_EQ(*dec.u32(), 42u);  // xid
  (void)dec.u32();             // REPLY
  (void)dec.u32();             // MSG_ACCEPTED
  (void)dec.u32();             // verf flavor
  (void)dec.u32();             // verf len
  EXPECT_EQ(*dec.u32(), 0u);   // SUCCESS
  auto echoed = *dec.opaque(1 << 16);
  EXPECT_STREQ(std::string(reinterpret_cast<const char*>(echoed.data()), echoed.size()),
               "over-tls");

  SSL_shutdown(ssl);
  SSL_free(ssl);
  SSL_CTX_free(cctx);
  close(fd);
}

TEST(Tls, OptionalStillServesCleartext) {
  TempCert cert;
  ASSERT_TRUE(cert.ok);
  Server srv(TlsPolicy::kOptional, cert);
  int fd = connect_loopback(srv.port());
  ASSERT_TRUE(fd >= 0);
  // No probe: a plain echo must still be answered in cleartext.
  auto call = build_call(7, 1, 0, "clear");
  ASSERT_TRUE(raw_write(fd, call.data(), call.size()));
  std::vector<std::byte> rep;
  ASSERT_TRUE(read_record([&](std::byte* p, size_t n) { return raw_read(fd, p, n); }, rep));
  xdr::XdrDec dec{std::span<const std::byte>(rep.data(), rep.size())};
  EXPECT_EQ(*dec.u32(), 7u);
  close(fd);
}

TEST(Tls, RequiredRejectsCleartextOps) {
  TempCert cert;
  ASSERT_TRUE(cert.ok);
  Server srv(TlsPolicy::kRequired, cert);
  int fd = connect_loopback(srv.port());
  ASSERT_TRUE(fd >= 0);
  // A cleartext echo (proc 1) on a required-TLS server is denied AUTH_TOOWEAK.
  auto call = build_call(9, 1, 0, "nope");
  ASSERT_TRUE(raw_write(fd, call.data(), call.size()));
  std::vector<std::byte> rep;
  ASSERT_TRUE(read_record([&](std::byte* p, size_t n) { return raw_read(fd, p, n); }, rep));
  xdr::XdrDec dec{std::span<const std::byte>(rep.data(), rep.size())};
  EXPECT_EQ(*dec.u32(), 9u);   // xid
  (void)dec.u32();             // REPLY
  EXPECT_EQ(*dec.u32(), 1u);   // MSG_DENIED
  EXPECT_EQ(*dec.u32(), 1u);   // AUTH_ERROR
  EXPECT_EQ(*dec.u32(), 5u);   // AUTH_TOOWEAK
  close(fd);

  // The STARTTLS probe (a NULL) is still accepted, and TLS then works.
  fd = connect_loopback(srv.port());
  ASSERT_TRUE(fd >= 0);
  ASSERT_TRUE(do_starttls_probe(fd));
  SSL_CTX* cctx = client_ctx();
  SSL* ssl = SSL_new(cctx);
  SSL_set_fd(ssl, fd);
  ASSERT_TRUE(SSL_connect(ssl) == 1);
  auto call2 = build_call(10, 1, 0, "ok-now");
  ASSERT_TRUE(ssl_write_all(ssl, call2.data(), call2.size()));
  std::vector<std::byte> rep2;
  ASSERT_TRUE(read_record([&](std::byte* p, size_t n) { return ssl_read_all(ssl, p, n); },
                          rep2));
  xdr::XdrDec d2{std::span<const std::byte>(rep2.data(), rep2.size())};
  EXPECT_EQ(*d2.u32(), 10u);
  SSL_shutdown(ssl);
  SSL_free(ssl);
  SSL_CTX_free(cctx);
  close(fd);
}

TEST(Tls, OffDeclinesProbeAndStaysCleartext) {
  TempCert cert;
  Server srv(TlsPolicy::kOff, cert);
  int fd = connect_loopback(srv.port());
  ASSERT_TRUE(fd >= 0);
  // The probe is answered as an ordinary NULL reply (AUTH_NONE verifier, no STARTTLS).
  auto probe = build_call(3, 0, kAuthTls, "");
  ASSERT_TRUE(raw_write(fd, probe.data(), probe.size()));
  std::vector<std::byte> rep;
  ASSERT_TRUE(read_record([&](std::byte* p, size_t n) { return raw_read(fd, p, n); }, rep));
  xdr::XdrDec dec{std::span<const std::byte>(rep.data(), rep.size())};
  EXPECT_EQ(*dec.u32(), 3u);  // xid
  (void)dec.u32();            // REPLY
  (void)dec.u32();            // MSG_ACCEPTED
  EXPECT_EQ(*dec.u32(), 0u);  // verf flavor AUTH_NONE (declined)
  EXPECT_EQ(*dec.u32(), 0u);  // verf len 0
  EXPECT_EQ(*dec.u32(), 0u);  // SUCCESS

  // The connection stays usable in cleartext.
  auto call = build_call(4, 1, 0, "still-here");
  ASSERT_TRUE(raw_write(fd, call.data(), call.size()));
  std::vector<std::byte> rep2;
  ASSERT_TRUE(read_record([&](std::byte* p, size_t n) { return raw_read(fd, p, n); }, rep2));
  xdr::XdrDec d2{std::span<const std::byte>(rep2.data(), rep2.size())};
  EXPECT_EQ(*d2.u32(), 4u);
  close(fd);
}

TEST(Tls, ConfigSectionParsesAndValidates) {
  TempCert cert;
  ASSERT_TRUE(cert.ok);
  // A valid export so validate_config's failures are attributable to the [tls] section
  // rather than to an empty export table.
  const std::string exp =
      "\n[[export]]\npath = \"/tmp\"\nbackend = \"local\"\nfsid = 1\n"
      "clients = [\"127.0.0.0/8\"]\n";

  std::string good = "[server]\nstate_dir = \"/tmp\"\n[tls]\nmode = \"required\"\ncert = \"" +
                     cert.cert + "\"\nkey = \"" + cert.key + "\"\n" + exp;
  auto ok = core::parse_config(good);
  ASSERT_TRUE(ok.has_value());
  EXPECT_STREQ(ok->server.tls_mode, "required");
  EXPECT_STREQ(ok->server.tls_cert, cert.cert);
  EXPECT_TRUE(core::validate_config(*ok).has_value());  // real cert files: valid

  // A non-off mode with no cert/key is rejected by validate_config.
  auto no_cert = core::parse_config("[tls]\nmode = \"optional\"\n" + exp);
  ASSERT_TRUE(no_cert.has_value());
  EXPECT_FALSE(core::validate_config(*no_cert).has_value());

  // A bad mode literal is rejected.
  auto bad_mode = core::parse_config("[tls]\nmode = \"yes\"\ncert = \"" + cert.cert +
                                     "\"\nkey = \"" + cert.key + "\"\n" + exp);
  ASSERT_TRUE(bad_mode.has_value());
  EXPECT_FALSE(core::validate_config(*bad_mode).has_value());

  // client_cert without a CA bundle (mutual TLS needs one) is rejected.
  auto no_ca = core::parse_config("[tls]\nmode = \"optional\"\nclient_cert = true\ncert = \"" +
                                  cert.cert + "\"\nkey = \"" + cert.key + "\"\n" + exp);
  ASSERT_TRUE(no_ca.has_value());
  EXPECT_FALSE(core::validate_config(*no_ca).has_value());

  // An unknown [tls] key is a parse error.
  EXPECT_FALSE(core::parse_config("[tls]\nbogus = 1\n").has_value());
}

#else  // no OpenSSL at build time: nothing to test (config still rejects non-off modes).

TEST(Tls, DisabledBuildRejectsNonOffMode) {
  auto cfg = lnfs::core::parse_config("[tls]\nmode = \"optional\"\n");
  ASSERT_TRUE(cfg.has_value());
  EXPECT_FALSE(lnfs::core::validate_config(*cfg).has_value());
}

#endif
