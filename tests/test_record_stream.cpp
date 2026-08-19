// RecordStream framing over FakeRing: multi-fragment reassembly, short reads, EINTR,
// oversize -> error, partial sends on the write path.

#include "mini_test.hpp"
#include "runtime/reactor.hpp"
#include "runtime/testing/fake_ring.hpp"
#include "transport/record_stream.hpp"
#include "xdr/xdr.hpp"

using namespace lnfs;
using namespace lnfs::rt;
using namespace lnfs::transport;
using testing::FakeRing;

namespace {

std::vector<std::byte> frag(std::string_view payload, bool last) {
  std::vector<std::byte> out(4 + payload.size());
  uint32_t hdr = xdr::to_be32((last ? 0x80000000u : 0) | (uint32_t)payload.size());
  std::memcpy(out.data(), &hdr, 4);
  std::memcpy(out.data() + 4, payload.data(), payload.size());
  return out;
}

std::vector<std::byte> cat(std::initializer_list<std::vector<std::byte>> parts) {
  std::vector<std::byte> out;
  for (auto& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

struct Fixture {
  FakeRing ring;
  Reactor r{ring};
  BufferPool pool;
  RecordStream rs{5, pool, 1 << 20, (1 << 20) + (64 << 10)};

  Result<BufferChain> result = Err(Errno::kOk);
  bool done = false;

  void start_read() {
    spawn(
        [](Fixture* f) -> Task<void> {
          f->result = co_await f->rs.read_record();
          f->done = true;
        }(this),
        r);
    pump();
  }
  void pump() {
    while (r.poll_once()) {
    }
  }
  void feed(std::span<const std::byte> bytes) {
    auto op = ring.take(FakeRing::Kind::kRecv, 5);
    ring.complete_with_data(op, bytes);
    pump();
  }
};

}  // namespace

TEST(RecordStream, SingleFragment) {
  Fixture f;
  f.start_read();
  f.feed(frag("hello rpc", true));
  ASSERT_TRUE(f.done);
  ASSERT_TRUE(f.result.has_value());
  auto bytes = f.result->to_bytes();
  EXPECT_STREQ(std::string((char*)bytes.data(), bytes.size()), "hello rpc");
}

TEST(RecordStream, MultiFragmentReassembly) {
  Fixture f;
  f.start_read();
  f.feed(cat({frag("part1-", false), frag("part2", true)}));
  ASSERT_TRUE(f.done);
  ASSERT_TRUE(f.result.has_value());
  auto bytes = f.result->to_bytes();
  EXPECT_STREQ(std::string((char*)bytes.data(), bytes.size()), "part1-part2");
}

TEST(RecordStream, ShortReadsAndEintr) {
  Fixture f;
  f.start_read();
  auto whole = frag("abcdefgh", true);
  // deliver 3 bytes, then EINTR, then the rest byte-by-byte over two more recvs
  f.feed(std::span(whole).subspan(0, 3));
  f.ring.complete(f.ring.take(FakeRing::Kind::kRecv, 5), -EINTR);
  f.pump();
  f.feed(std::span(whole).subspan(3, 5));
  f.feed(std::span(whole).subspan(8));
  ASSERT_TRUE(f.done);
  ASSERT_TRUE(f.result.has_value());
  EXPECT_EQ(f.result->size(), 8u);
}

TEST(RecordStream, EofBetweenRecords) {
  Fixture f;
  f.start_read();
  f.ring.complete(f.ring.take(FakeRing::Kind::kRecv, 5), 0);  // orderly EOF
  f.pump();
  ASSERT_TRUE(f.done);
  EXPECT_FALSE(f.result.has_value());
  EXPECT_EQ((int)f.result.error(), (int)Errno::kEof);
}

TEST(RecordStream, EofMidRecordIsFramingError) {
  Fixture f;
  f.start_read();
  auto whole = frag("abcdefgh", true);
  f.feed(std::span(whole).subspan(0, 6));  // header + 2 payload bytes
  f.ring.complete(f.ring.take(FakeRing::Kind::kRecv, 5), 0);
  f.pump();
  ASSERT_TRUE(f.done);
  EXPECT_FALSE(f.result.has_value());
  EXPECT_EQ((int)f.result.error(), EBADMSG);
}

TEST(RecordStream, OversizeFragmentRejected) {
  FakeRing ring;
  Reactor r{ring};
  BufferPool pool;
  RecordStream rs{5, pool, 1024, 2048};  // small caps
  Result<BufferChain> res = Err(Errno::kOk);
  bool done = false;
  spawn(
      [](RecordStream* s, Result<BufferChain>* out, bool* d) -> Task<void> {
        *out = co_await s->read_record();
        *d = true;
      }(&rs, &res, &done),
      r);
  while (r.poll_once()) {
  }
  uint32_t hdr = xdr::to_be32(0x80000000u | 4096u);  // fragment larger than cap
  ring.complete_with_data(ring.take(FakeRing::Kind::kRecv, 5),
                          std::span<const std::byte>((std::byte*)&hdr, 4));
  while (r.poll_once()) {
  }
  ASSERT_TRUE(done);
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ((int)res.error(), EMSGSIZE);
}

TEST(RecordStream, WritePartialSendContinues) {
  Fixture f;
  xdr::XdrEnc enc(f.pool);
  enc.string("0123456789abcdef");
  bool ok = false;
  spawn(
      [](Fixture* ff, SendBuf b, bool* okk) -> Task<void> {
        auto r = co_await ff->rs.write_record(std::move(b));
        *okk = r.has_value();
      }(&f, enc.take(), &ok),
      f.r);
  f.pump();
  // total = 4 marker + 4 len + 16 data = 24; complete in two partial sends
  auto op1 = f.ring.take(FakeRing::Kind::kSendv, 5);
  size_t total1 = 0;
  for (int i = 0; i < op1.iovcnt; ++i) total1 += op1.iov[i].iov_len;
  EXPECT_EQ(total1, 24u);
  f.ring.complete(op1, 10);  // partial
  f.pump();
  auto op2 = f.ring.take(FakeRing::Kind::kSendv, 5);
  size_t total2 = 0;
  for (int i = 0; i < op2.iovcnt; ++i) total2 += op2.iov[i].iov_len;
  EXPECT_EQ(total2, 14u);  // remainder
  f.ring.complete(op2, 14);
  f.pump();
  EXPECT_TRUE(ok);
  EXPECT_EQ(f.rs.send_queued_bytes(), 0u);
}
