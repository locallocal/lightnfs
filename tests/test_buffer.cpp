#include "mini_test.hpp"
#include "runtime/buffer.hpp"
#include <thread>
#include <vector>

using namespace lnfs::rt;

TEST(BufferPool, SizeClassesAndReuse) {
  BufferPool pool;
  auto b1 = pool.alloc(100);
  EXPECT_EQ(b1.capacity(), BufferPool::kSmall);
  auto b2 = pool.alloc(5000);
  EXPECT_EQ(b2.capacity(), BufferPool::kMedium);
  // Intermediate classes (plan doc 10 §2.4): typical rsize allocations no longer round
  // all the way to the 1M class.
  auto b3 = pool.alloc(70000);
  EXPECT_EQ(b3.capacity(), 128u * 1024);
  auto b3b = pool.alloc(200000);
  EXPECT_EQ(b3b.capacity(), 256u * 1024);
  auto b3c = pool.alloc(300000);
  EXPECT_EQ(b3c.capacity(), BufferPool::kLarge);
  std::byte* p1 = b1.data();
  b1 = Buffer();  // release -> this thread's magazine
  auto b4 = pool.alloc(50);
  EXPECT_TRUE(b4.data() == p1);  // reused without touching the global freelist
}

TEST(BufferPool, OversizeUnpooled) {
  BufferPool pool;
  auto b = pool.alloc(3 * 1024 * 1024);
  EXPECT_EQ(b.capacity(), 3u * 1024 * 1024);
  b = Buffer();
  EXPECT_EQ(pool.free_bytes(), 0u);  // not cached
}

TEST(BufferPool, MagazineOverflowSpillsToGlobal) {
  BufferPool pool;
  // Fill this thread's magazine past its cap; the overflow lands on the freelist.
  std::vector<Buffer> held;
  for (uint32_t i = 0; i < BufferPool::kMagazineCap + 3; ++i) held.push_back(pool.alloc(10));
  held.clear();
  EXPECT_EQ(pool.free_bytes(), 3u * BufferPool::kSmall);
}

TEST(BufferPool, Watermark) {
  BufferPool pool(BufferPool::Config{.max_free_bytes = BufferPool::kSmall});
  // Overflow the magazine so releases reach the global freelist, which caps at one
  // small block; the excess is freed outright.
  std::vector<Buffer> held;
  for (uint32_t i = 0; i < BufferPool::kMagazineCap + 4; ++i) held.push_back(pool.alloc(10));
  held.clear();
  EXPECT_EQ(pool.free_bytes(), BufferPool::kSmall);
}

TEST(BufferPool, CrossThreadRecycle) {
  BufferPool pool;
  auto b = pool.alloc(10);
  std::thread t([moved = std::move(b)]() mutable { moved = Buffer(); });
  t.join();  // freed on the other thread; its magazine flushed back at thread exit
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
