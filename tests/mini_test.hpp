#pragma once
// Minimal test framework (no external deps; swappable for GoogleTest later without touching
// test bodies much: TEST/EXPECT_* macros follow the same shape).

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace minitest {

struct Case {
  const char* name;
  void (*fn)();
};
inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}
inline int& failures() {
  static int f = 0;
  return f;
}
struct Register {
  Register(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};
inline int run_all(const char* filter = nullptr) {
  int ran = 0;
  for (auto& c : registry()) {
    if (filter && std::string(c.name).find(filter) == std::string::npos) continue;
    int before = failures();
    std::printf("[ RUN  ] %s\n", c.name);
    c.fn();
    ++ran;
    std::printf(failures() == before ? "[  OK  ] %s\n" : "[ FAIL ] %s\n", c.name);
  }
  std::printf("%d test(s) ran, %d failure(s)\n", ran, failures());
  return failures() ? 1 : 0;
}

}  // namespace minitest

#define TEST(suite, name)                                                        \
  static void suite##_##name##_body();                                           \
  static ::minitest::Register suite##_##name##_reg(#suite "." #name,             \
                                                   &suite##_##name##_body);      \
  static void suite##_##name##_body()

#define MT_FAIL(msg, ...)                                                        \
  do {                                                                           \
    ++::minitest::failures();                                                    \
    std::printf("  FAILED %s:%d: " msg "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
  } while (0)

#define EXPECT_TRUE(c)                    \
  do {                                    \
    if (!(c)) MT_FAIL("EXPECT_TRUE(%s)", #c); \
  } while (0)
#define EXPECT_FALSE(c)                     \
  do {                                      \
    if ((c)) MT_FAIL("EXPECT_FALSE(%s)", #c); \
  } while (0)
#define EXPECT_EQ(a, b)                                                             \
  do {                                                                              \
    auto _a = (a);                                                                  \
    auto _b = (b);                                                                  \
    if (!(_a == _b)) MT_FAIL("EXPECT_EQ(%s, %s): %lld vs %lld", #a, #b,             \
                             (long long)(_a), (long long)(_b));                     \
  } while (0)
#define EXPECT_STREQ(a, b)                                              \
  do {                                                                  \
    std::string _a = (a);                                               \
    std::string _b = (b);                                               \
    if (_a != _b) MT_FAIL("EXPECT_STREQ: \"%s\" vs \"%s\"", _a.c_str(), _b.c_str()); \
  } while (0)
#define ASSERT_TRUE(c)                        \
  do {                                        \
    if (!(c)) {                               \
      MT_FAIL("ASSERT_TRUE(%s)", #c);         \
      return;                                 \
    }                                         \
  } while (0)
