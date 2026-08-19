#pragma once
// TCP record marking stream (design 03 §3.2). One instance per connection.
//  - read_record(): 4-byte marker state machine, fragment/total length caps, reassembles a
//    BufferChain of zero-copy slices of the recv buffers. Framing violations are fatal to
//    the connection (Err(EMSGSIZE/EBADMSG)); orderly EOF is Err(kEof).
//  - write_record(): per-connection serialization (AsyncMutex), partial-send continuation.

#include "runtime/buffer.hpp"
#include "runtime/io.hpp"
#include "runtime/sync.hpp"
#include "runtime/task.hpp"
#include "util/result.hpp"

namespace lnfs::transport {

class RecordStream {
 public:
  RecordStream(int fd, rt::BufferPool& pool, uint32_t max_fragment, uint32_t max_record)
      : fd_(fd), pool_(pool), max_fragment_(max_fragment), max_record_(max_record) {}

  rt::Task<Result<rt::BufferChain>> read_record();
  rt::Task<Result<void>> write_record(rt::SendBuf buf);

  size_t send_queued_bytes() const { return send_queued_; }

 private:
  rt::Task<Result<void>> fill();          // recv more bytes into rbuf_
  rt::Task<Result<uint32_t>> read_be32();  // may straddle recv buffers

  int fd_;
  rt::BufferPool& pool_;
  rt::Buffer rbuf_;
  uint32_t roff_ = 0;  // consumed
  uint32_t rend_ = 0;  // filled
  uint32_t max_fragment_;
  uint32_t max_record_;

  rt::AsyncMutex wmu_;
  size_t send_queued_ = 0;
};

}  // namespace lnfs::transport
