#include "transport/record_stream.hpp"

#include <cstring>

#include "xdr/xdr.hpp"

namespace lnfs::transport {

using namespace lnfs::rt;

Task<Result<void>> RecordStream::fill() {
  if (!rbuf_ || rend_ == rbuf_.capacity()) {
    try {
      rbuf_ = pool_.alloc(BufferPool::kMedium);
    } catch (const std::bad_alloc&) {
      co_return Err(errno_from(ENOMEM));  // OOM degrades to a connection error (§2.4)
    }
    roff_ = rend_ = 0;
  }
  for (;;) {
    int n = co_await uring_recv(
        fd_, std::span<std::byte>(rbuf_.data() + rend_, rbuf_.capacity() - rend_));
    if (n == -EINTR || n == -EAGAIN) continue;
    if (n == 0) co_return Err(Errno::kEof);
    if (n < 0) co_return Err(errno_from_neg(n));
    rend_ += static_cast<uint32_t>(n);
    co_return Result<void>{};
  }
}

Task<Result<uint32_t>> RecordStream::read_be32() {
  std::byte b[4];
  for (int i = 0; i < 4; ++i) {
    if (roff_ == rend_) {
      auto r = co_await fill();
      if (!r) co_return Err(r.error());
    }
    b[i] = rbuf_.data()[roff_++];
  }
  uint32_t v;
  std::memcpy(&v, b, 4);
  co_return xdr::from_be32(v);
}

Task<Result<rt::BufferChain>> RecordStream::read_record() {
  BufferChain rec;
  for (;;) {
    auto hdr = co_await read_be32();
    if (!hdr) {
      // EOF before the first marker byte of a record is an orderly close; mid-record EOF
      // is a framing error either way — both close the connection upstream.
      co_return Err(hdr.error());
    }
    const bool last = *hdr & 0x80000000u;
    const uint32_t len = *hdr & 0x7fffffffu;
    if (len > max_fragment_ || rec.size() + len > max_record_) {
      co_return Err(errno_from(EMSGSIZE));
    }
    uint32_t need = len;
    while (need > 0) {
      if (roff_ == rend_) {
        auto r = co_await fill();
        if (!r) co_return Err(r.error() == Errno::kEof ? errno_from(EBADMSG) : r.error());
      }
      uint32_t k = std::min(need, rend_ - roff_);
      rec.append(rbuf_, roff_, k);  // zero-copy slice; refcount keeps rbuf_ alive
      roff_ += k;
      need -= k;
    }
    if (last) break;
  }
  co_return rec;
}

Task<Result<void>> RecordStream::write_record(SendBuf buf) {
  const size_t total = buf.size();
  send_queued_ += total;
  auto lk = co_await wmu_.lock();  // per-conn reply serialization (design 03 §3.2)

  // 4-byte record mark (single fragment; RPC replies are bounded by reply-size budgets).
  uint32_t marker = xdr::to_be32(0x80000000u | static_cast<uint32_t>(total));
  SmallVec<iovec, 8> iov;   // typical replies: marker + a few segments — no heap (§2.4)
  SmallVec<iovec, 8> body;
  size_t sent = 0;
  while (sent < 4 + total) {
    iov.clear();
    if (sent < 4) {
      iov.push_back(iovec{reinterpret_cast<std::byte*>(&marker) + sent, 4 - sent});
      buf.to_iovecs(body, 0);
      for (const auto& v : body) iov.push_back(v);
    } else {
      buf.to_iovecs(iov, sent - 4);
    }
    int n = co_await uring_sendv(fd_, iov.data(), static_cast<int>(iov.size()));
    if (n == -EINTR || n == -EAGAIN) continue;
    if (n <= 0) {
      send_queued_ -= total;
      co_return Err(n == 0 ? errno_from(EPIPE) : errno_from_neg(n));
    }
    sent += static_cast<size_t>(n);
  }
  send_queued_ -= total;
  co_return Result<void>{};
}

}  // namespace lnfs::transport
