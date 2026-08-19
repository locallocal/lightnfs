#include "mini_test.hpp"
#include "runtime/buffer.hpp"

using namespace lnfs::rt;

TEST(BufferPool, SizeClassesAndReuse) {
  BufferPool pool;
  auto b1 = pool.alloc(100);
  EXPECT_EQ(b1.capacity(), BufferPool::kSmall);
  auto b2 = pool.alloc(5000);
  EXPECT_EQ(b2.capacity(), BufferPool::kMedium);
  auto b3 = pool.alloc(70000);
  EXPECT_EQ(b3.capacity(), BufferPool::kLarge);
  std::byte* p1 = b1.data();
  b1 = Buffer();  // release -> freelist
  EXPECT_TRUE(pool.free_bytes() >= BufferPool::kSmall);
  auto b4 = pool.alloc(50);
  EXPECT_TRUE(b4.data() == p1);  // reused
}

TEST(BufferPool, OversizeUnpooled) {
  BufferPool pool;
  auto b = pool.alloc(3 * 1024 * 1024);
  EXPECT_EQ(b.capacity(), 3u * 1024 * 1024);
  b = Buffer();
  EXPECT_EQ(pool.free_bytes(), 0u);  // not cached
}

TEST(BufferPool, Watermark) {
  BufferPool pool(BufferPool::Config{.max_free_bytes = BufferPool::kSmall});
  auto a = pool.alloc(10);
  auto b = pool.alloc(10);
  a = Buffer();
  b = Buffer();  // second free exceeds watermark -> freed, not cached
  EXPECT_EQ(pool.free_bytes(), BufferPool::kSmall);
}

TEST(BufferChain, RefcountKeepsDataAlive) {
  BufferPool pool;
  BufferChain chain;
  {
    auto b = pool.alloc(16);
    std::memcpy(b.data(), "hello world!", 12);
    chain.append(b, 0, 5);
    chain.append(b, 6, 5);
  }  // local Buffer released; chain still holds refs
  EXPECT_EQ(chain.size(), 10u);
  auto bytes = chain.to_bytes();
  EXPECT_STREQ(std::string(reinterpret_cast<char*>(bytes.data()), bytes.size()), "helloworld");
}

TEST(BufferChain, IovecsWithSkip) {
  BufferPool pool;
  BufferChain chain;
  auto b = pool.alloc(16);
  std::memcpy(b.data(), "abcdefgh", 8);
  chain.append(b, 0, 4);
  chain.append(b, 4, 4);
  std::vector<iovec> iov;
  chain.to_iovecs(iov, 6);  // skip first seg entirely + 2 bytes of second
  ASSERT_TRUE(iov.size() == 1);
  EXPECT_EQ(iov[0].iov_len, 2u);
  EXPECT_EQ(static_cast<char*>(iov[0].iov_base)[0], 'g');
}
