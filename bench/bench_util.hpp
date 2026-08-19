#pragma once
// Shared client-side load driver for the layer benchmarks (design 02 §2.8): blocking-socket
// client threads blast pipelined records at a server and count completed round-trips.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "xdr/xdr.hpp"

namespace lnfs::bench {

inline int connect_loopback(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    perror("connect");
    close(fd);
    return -1;
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return fd;
}

inline bool read_exact(int fd, std::byte* p, size_t n) {
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r <= 0) return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

// Reads and discards one record; returns false on EOF/error.
inline bool drain_record(int fd) {
  std::byte hdr[4];
  for (;;) {
    if (!read_exact(fd, hdr, 4)) return false;
    uint32_t h;
    std::memcpy(&h, hdr, 4);
    h = xdr::from_be32(h);
    size_t len = h & 0x7fffffffu;
    static thread_local std::vector<std::byte> sink;
    sink.resize(len);
    if (!read_exact(fd, sink.data(), len)) return false;
    if (h & 0x80000000u) return true;
  }
}

struct LoadResult {
  uint64_t completed = 0;
  double seconds = 0;
  double rps() const { return completed / seconds; }
};

// conns client connections, each sending `per_conn` copies of `record` with `pipeline`
// requests outstanding.
inline LoadResult run_load(uint16_t port, int conns, uint64_t per_conn, int pipeline,
                           const std::vector<std::byte>& record) {
  std::atomic<uint64_t> completed{0};
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;
  for (int c = 0; c < conns; ++c) {
    threads.emplace_back([&, c] {
      int fd = connect_loopback(port);
      if (fd < 0) return;
      uint64_t sent = 0, recvd = 0;
      while (recvd < per_conn) {
        while (sent < per_conn && sent - recvd < static_cast<uint64_t>(pipeline)) {
          if (write(fd, record.data(), record.size()) != (ssize_t)record.size()) goto out;
          ++sent;
        }
        if (!drain_record(fd)) goto out;
        ++recvd;
        completed.fetch_add(1, std::memory_order_relaxed);
      }
    out:
      close(fd);
    });
  }
  for (auto& t : threads) t.join();
  auto t1 = std::chrono::steady_clock::now();
  LoadResult r;
  r.completed = completed.load();
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  return r;
}

}  // namespace lnfs::bench
