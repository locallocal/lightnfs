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
