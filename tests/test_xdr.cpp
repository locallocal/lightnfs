// XDR round-trips, bounds -> kGarbage, padding, zero-copy spans, raw_gap patching.

#include "mini_test.hpp"
#include "xdr/xdr.hpp"

using namespace lnfs;
using namespace lnfs::xdr;

namespace {
rt::BufferChain enc_to_chain(XdrEnc& enc) { return enc.take(); }

rt::BufferChain make_chain(rt::BufferPool& pool, std::span<const std::byte> bytes,
                           size_t split) {
  // Build a chain split into two segments to exercise the spanning path.
  rt::BufferChain c;
  auto b = pool.alloc(bytes.size() ? bytes.size() : 1);
  std::memcpy(b.data(), bytes.data(), bytes.size());
  split = std::min(split, bytes.size());
  c.append(b, 0, static_cast<uint32_t>(split));
  c.append(b, static_cast<uint32_t>(split), static_cast<uint32_t>(bytes.size() - split));
  return c;
}
}  // namespace

TEST(Xdr, RoundTripScalars) {
  rt::BufferPool pool;
  XdrEnc enc(pool);
  enc.u32(0xdeadbeef);
  enc.u64(0x0123456789abcdefULL);
  enc.boolean(true);
  enc.string("hello");
  auto chain = enc_to_chain(enc);
  EXPECT_EQ(chain.size(), 4u + 8 + 4 + 4 + 8);  // "hello" -> 5 + 3 pad

  XdrDec dec(chain);
  EXPECT_EQ(*dec.u32(), 0xdeadbeefu);
  EXPECT_EQ(*dec.u64(), 0x0123456789abcdefULL);
  EXPECT_TRUE(*dec.boolean());
  EXPECT_STREQ(std::string(*dec.string(64)), "hello");
  EXPECT_TRUE(dec.at_end());
}

TEST(Xdr, BigEndianOnWire) {
  rt::BufferPool pool;
  XdrEnc enc(pool);
  enc.u32(1);
  auto bytes = enc_to_chain(enc).to_bytes();
  EXPECT_EQ((int)bytes[0], 0);
  EXPECT_EQ((int)bytes[3], 1);
}

TEST(Xdr, OpaqueMaxViolation) {
  rt::BufferPool pool;
  XdrEnc enc(pool);
  enc.opaque(std::span<const std::byte>(reinterpret_cast<const std::byte*>("abcdef"), 6));
  auto chain = enc_to_chain(enc);
  XdrDec dec(chain);
  auto r = dec.opaque(4);  // max 4 < 6
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ((int)r.error(), (int)Errno::kGarbage);
}

TEST(Xdr, TruncatedInput) {
  rt::BufferPool pool;
  XdrEnc enc(pool);
  enc.u32(100);  // claims 100-byte opaque, then nothing
  auto chain = enc_to_chain(enc);
  XdrDec dec(chain);
  auto r = dec.opaque(1000);
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ((int)r.error(), (int)Errno::kGarbage);
}

TEST(Xdr, SpanningSegmentsGather) {
  rt::BufferPool pool;
  XdrEnc enc(pool);
  enc.u64(0x1122334455667788ULL);
  auto flat = enc_to_chain(enc).to_bytes();
  auto chain = make_chain(pool, flat, 3);  // u64 straddles the segment boundary
  EXPECT_EQ(chain.seg_count(), 2u);
  XdrDec dec(chain);
  EXPECT_EQ(*dec.u64(), 0x1122334455667788ULL);
}

TEST(Xdr, ZeroCopyOpaqueReferencesChain) {
  rt::BufferPool pool;
  XdrEnc enc(pool);
  std::vector<std::byte> data(64, std::byte{0x5a});
  enc.opaque(data);
  auto chain = enc_to_chain(enc);
  XdrDec dec(chain);
  auto sp = *dec.opaque(1 << 20);
  EXPECT_EQ(sp.size(), 64u);
  // zero-copy: the span must point into the chain's buffer, not scratch
  const std::byte* base = chain.seg(0).buf.data();
  EXPECT_TRUE(sp.data() >= base && sp.data() < base + chain.seg(0).buf.capacity());
}

TEST(Xdr, RawGapPatch) {
  rt::BufferPool pool;
  XdrEnc enc(pool);
  enc.u32(7);
  std::byte* gap = enc.raw_gap(4);
  enc.u32(9);
  uint32_t be = to_be32(0x42);
  std::memcpy(gap, &be, 4);  // patch after later fields were written
  auto chain = enc_to_chain(enc);
  XdrDec dec(chain);
  EXPECT_EQ(*dec.u32(), 7u);
  EXPECT_EQ(*dec.u32(), 0x42u);
  EXPECT_EQ(*dec.u32(), 9u);
}

TEST(Xdr, AttachZeroCopySegment) {
  rt::BufferPool pool;
  XdrEnc enc(pool);
  auto data = pool.alloc(10);
  std::memcpy(data.data(), "0123456789", 10);
  enc.u32(10);           // opaque length
  enc.attach(data, 0, 10);  // spliced, padded to 12
  enc.u32(0xff);
  auto chain = enc_to_chain(enc);
  EXPECT_TRUE(chain.seg_count() >= 3);  // head, attached, pad+tail
  XdrDec dec(chain);
  auto sp = *dec.opaque(64);
  EXPECT_EQ(sp.size(), 10u);
  EXPECT_EQ(*dec.u32(), 0xffu);
}

TEST(Xdr, FlatSpanMode) {
  const unsigned char raw[] = {0, 0, 0, 5, 0, 0, 0, 2};
  XdrDec dec(std::span<const std::byte>(reinterpret_cast<const std::byte*>(raw), 8));
  EXPECT_EQ(*dec.u32(), 5u);
  EXPECT_EQ(*dec.u32(), 2u);
  EXPECT_FALSE(dec.u32().has_value());
}

// ---- speculative encoding (plan doc 10 §2.4) --------------------------------

TEST(Xdr, MarkRollbackSameTail) {
  rt::BufferPool pool;
  XdrEnc enc(pool);
  enc.u32(1);
  auto m = enc.mark();
  enc.u32(2);
  enc.u32(3);
  enc.rollback(m);
  enc.u32(4);
  auto chain = enc_to_chain(enc);
  XdrDec dec(chain);
  EXPECT_EQ(*dec.u32(), 1u);
  EXPECT_EQ(*dec.u32(), 4u);
  EXPECT_TRUE(dec.at_end());
}

TEST(Xdr, MarkRollbackAcrossTailClose) {
  rt::BufferPool pool;
  XdrEnc enc(pool);
  enc.u32(7);
  auto m = enc.mark();
  // Overflow the 4K tail so it is closed into the chain and a fresh tail opens.
  std::vector<std::byte> big(rt::BufferPool::kSmall + 100, std::byte{0x5a});
  enc.opaque_fixed(big);
  enc.u32(9);
  enc.rollback(m);
  enc.u32(8);
  auto chain = enc_to_chain(enc);
  EXPECT_EQ(chain.size(), 8u);
  XdrDec dec(chain);
  EXPECT_EQ(*dec.u32(), 7u);
  EXPECT_EQ(*dec.u32(), 8u);
}

TEST(Xdr, MarkRollbackAcrossAttach) {
  rt::BufferPool pool;
  XdrEnc enc(pool);
  enc.u32(1);
  auto m = enc.mark();
  auto data = pool.alloc(6);
  std::memcpy(data.data(), "abcdef", 6);
  enc.u32(6);
  enc.attach(data, 0, 6);  // closes the tail, splices a segment, pads
  enc.rollback(m);
  enc.u32(2);
  auto chain = enc_to_chain(enc);
  EXPECT_EQ(chain.size(), 8u);
  XdrDec dec(chain);
  EXPECT_EQ(*dec.u32(), 1u);
  EXPECT_EQ(*dec.u32(), 2u);
}

TEST(Xdr, MarkRollbackKeepsEarlierGapPatchable) {
  rt::BufferPool pool;
  XdrEnc enc(pool);
  std::byte* gap = enc.raw_gap(4);
  auto m = enc.mark();
  enc.u32(0xdead);
  enc.rollback(m);
  enc.u32(0xbeef);
  uint32_t v = to_be32(42);
  std::memcpy(gap, &v, 4);  // gap pointer from before the mark stays valid
  auto chain = enc_to_chain(enc);
  XdrDec dec(chain);
  EXPECT_EQ(*dec.u32(), 42u);
  EXPECT_EQ(*dec.u32(), 0xbeefu);
}

TEST(Xdr, OpaqueSpansZeroCopyAcrossSegments) {
  rt::BufferPool pool;
  // opaque<9>: len=9, payload "abcdefghi", 3 pad bytes, then a trailing u32.
  std::vector<std::byte> raw;
  auto push32 = [&](uint32_t v) {
    v = to_be32(v);
    auto* p = reinterpret_cast<const std::byte*>(&v);
    raw.insert(raw.end(), p, p + 4);
  };
  push32(9);
  const char* payload = "abcdefghi";
  raw.insert(raw.end(), reinterpret_cast<const std::byte*>(payload),
             reinterpret_cast<const std::byte*>(payload) + 9);
  raw.insert(raw.end(), 3, std::byte{0});
  push32(0x77);
  // Split inside the payload so it spans two chain segments.
  auto chain = make_chain(pool, raw, 7);
  XdrDec dec(chain);
  lnfs::SmallVec<std::span<const std::byte>, 4> segs;
  auto len = dec.opaque_spans(64, segs);
  ASSERT_TRUE(len.has_value());
  EXPECT_EQ(*len, 9u);
  ASSERT_TRUE(segs.size() == 2);  // no gather: one span per chain segment
  std::string got;
  for (auto s : segs) got.append(reinterpret_cast<const char*>(s.data()), s.size());
  EXPECT_STREQ(got, "abcdefghi");
  EXPECT_EQ(*dec.u32(), 0x77u);  // padding was consumed
  EXPECT_TRUE(dec.at_end());
}

TEST(Xdr, OpaqueSpansRejectsOverMaxAndTruncated) {
  rt::BufferPool pool;
  std::vector<std::byte> raw;
  uint32_t v = to_be32(100);
  auto* p = reinterpret_cast<const std::byte*>(&v);
  raw.insert(raw.end(), p, p + 4);
  raw.insert(raw.end(), 4, std::byte{0x11});  // only 4 payload bytes present
  auto chain = make_chain(pool, raw, 3);
  {
    XdrDec dec(chain);
    lnfs::SmallVec<std::span<const std::byte>, 4> segs;
    EXPECT_FALSE(dec.opaque_spans(50, segs).has_value());  // len > max
  }
  {
    XdrDec dec(chain);
    lnfs::SmallVec<std::span<const std::byte>, 4> segs;
    EXPECT_FALSE(dec.opaque_spans(200, segs).has_value());  // len > remaining
  }
}
