// Fuzz target for RPC record-marking reassembly (plan doc 10 §7.2): the old request-path
// fuzzer fed complete records, so RecordStream's fragment parser (marker split across
// reads, multi-fragment records, oversize guards, EOF-mid-record) was never fuzzed.
// Here the input is a raw byte stream delivered through FakeRing recv completions in
// chunk sizes derived from the input itself.  Hermetic: fake ring, no sockets.

#include <cstdint>
#include <cstring>
#include <span>

#include "runtime/buffer.hpp"
#include "runtime/reactor.hpp"
#include "runtime/testing/fake_ring.hpp"
#include "transport/record_stream.hpp"
#include "util/log.hpp"

using namespace lnfs;
using namespace lnfs::rt;

extern "C" void lnfs_fuzz_entry(const uint8_t* data, size_t size) {
  [[maybe_unused]] static bool quiet = [] {
    lnfs::set_log_level(lnfs::LogLevel::kError);
    return true;
  }();
  testing::FakeRing ring;
  Reactor r(ring);
  BufferPool pool;
  // Small caps so oversize/overflow guards are reachable with fuzz-sized inputs.
  transport::RecordStream rs(5, pool, /*max_fragment=*/4096, /*max_record=*/8192);

  struct State {
    Result<BufferChain> result = Err(Errno::kOk);
    bool done = false;
    int records = 0;
  } st;
  auto start_read = [&] {
    st.done = false;
    spawn([](transport::RecordStream* rs, State* st) -> Task<void> {
      st->result = co_await rs->read_record();
      st->done = true;
    }(&rs, &st),
          r);
    while (r.poll_once()) {
    }
  };

  start_read();
  size_t pos = 0;
  int guard = 0;
  while (!st.done && ++guard < 4096) {
    if (!ring.has_pending(testing::FakeRing::Kind::kRecv, 5)) break;
    auto op = ring.take(testing::FakeRing::Kind::kRecv, 5);
    if (pos >= size) {
      ring.complete(op, 0);  // EOF
    } else {
      // Chunk size steered by the stream itself: 1..16 bytes per completion.
      size_t chunk = 1 + (data[pos] & 0x0f);
      chunk = std::min(chunk, size - pos);
      ring.complete_with_data(
          op, std::span<const std::byte>(reinterpret_cast<const std::byte*>(data + pos),
                                         chunk));
      pos += chunk;
    }
    while (r.poll_once()) {
    }
    if (st.done && st.result.has_value() && st.records < 64) {
      ++st.records;  // a complete record: keep reading the rest of the stream
      start_read();
    }
  }
}

#ifndef LNFS_FUZZ_REGRESS
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lnfs_fuzz_entry(data, size);
  return 0;
}
#endif
