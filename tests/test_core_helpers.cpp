// Unit tests for the core helpers introduced by the structural refactor
// (plan doc 10 §6): name component discipline, fs property derivation and the
// MutateGuard precheck verdicts.  Lock/sample behavior is covered end-to-end by
// test_nfs3/test_nfs4.

#include "mini_test.hpp"

#include <string_view>

#include "backend/memory.hpp"
#include "core/config.hpp"
#include "core/fs_props.hpp"
#include "core/mutate.hpp"
#include "core/names.hpp"
#include "core/obj_lock.hpp"

using namespace lnfs;
using core::NameCheck;

TEST(CoreNames, CheckComponentClassification) {
  EXPECT_TRUE(core::check_component("hello") == NameCheck::kOk);
  EXPECT_TRUE(core::check_component("") == NameCheck::kEmpty);
  EXPECT_TRUE(core::check_component(std::string(256, 'a')) == NameCheck::kTooLong);
  EXPECT_TRUE(core::check_component(std::string(255, 'a')) == NameCheck::kOk);
  EXPECT_TRUE(core::check_component("a/b") == NameCheck::kBadChar);
  EXPECT_TRUE(core::check_component(std::string_view("a\0b", 3)) == NameCheck::kBadChar);
  EXPECT_TRUE(core::check_component(".") == NameCheck::kDot);
  EXPECT_TRUE(core::check_component("..") == NameCheck::kDot);
  EXPECT_TRUE(core::check_component("...") == NameCheck::kOk);
}

TEST(CoreNames, ValidComponentDotPolicy) {
  EXPECT_TRUE(core::valid_component(".", /*allow_dots=*/true));   // v3 LOOKUP
  EXPECT_FALSE(core::valid_component(".", /*allow_dots=*/false)); // creation family
  EXPECT_FALSE(core::valid_component("a/b", true));
  EXPECT_TRUE(core::valid_component("regular", false));
}

TEST(CoreNames, ValidUtf8) {
  EXPECT_TRUE(core::valid_utf8(std::string_view("ascii")));
  EXPECT_TRUE(core::valid_utf8(std::string_view("\xc3\xa9")));          // é
  EXPECT_TRUE(core::valid_utf8(std::string_view("\xe4\xb8\xad")));      // 中
  EXPECT_TRUE(core::valid_utf8(std::string_view("\xf0\x9f\x98\x80")));  // emoji
  EXPECT_FALSE(core::valid_utf8(std::string_view("\xff")));
  EXPECT_FALSE(core::valid_utf8(std::string_view("\xc0\xaf")));          // overlong
  EXPECT_FALSE(core::valid_utf8(std::string_view("\xed\xa0\x80")));      // surrogate
  EXPECT_FALSE(core::valid_utf8(std::string_view("\xef\xbf\xbe")));      // U+FFFE
  EXPECT_FALSE(core::valid_utf8(std::string_view("\xc3")));              // truncated
}

TEST(CoreFsProps, DerivesAndClampsFromBackend) {
  backend::MemoryBackend mem(7);
  core::FsProps fs = core::fs_props(mem);
  auto limits = mem.limits();
  auto caps = mem.caps();
  EXPECT_EQ(fs.limits.max_read, limits.max_read);
  EXPECT_TRUE(fs.limits.pref_read <= fs.limits.max_read);
  EXPECT_TRUE(fs.limits.pref_write <= fs.limits.max_write);
  EXPECT_EQ(fs.link_support, caps.has(backend::Cap::kHardlink));
  EXPECT_EQ(fs.symlink_support, caps.has(backend::Cap::kSymlink));
  EXPECT_EQ(fs.case_insensitive, caps.has(backend::Cap::kCaseInsensitive));
  // Default-constructed = pseudo-fs answer: no link/symlink support.
  core::FsProps pseudo;
  EXPECT_FALSE(pseudo.link_support);
  EXPECT_FALSE(pseudo.symlink_support);
}

TEST(CoreMutate, PrecheckOrderAndVerdicts) {
  core::ObjLockRegistry locks;
  core::ExportTable exports;
  rpc::Cred cred;
  core::ExportEntry entry;

  core::MutateGuard rw(locks, exports, entry, cred);
  EXPECT_TRUE(static_cast<bool>(rw.precheck({})));
  EXPECT_TRUE(static_cast<bool>(rw.precheck({"a", "b"})));
  auto bad = rw.precheck({"ok-name", "with/slash"});
  EXPECT_FALSE(static_cast<bool>(bad));
  EXPECT_TRUE(bad.kind == core::MutateGuard::Verdict::kBadName);
  EXPECT_TRUE(bad.name == NameCheck::kBadChar);
  EXPECT_EQ(bad.name_index, 1u);
  auto dot = rw.precheck({".."});
  EXPECT_TRUE(dot.name == NameCheck::kDot);

  // Readonly is evaluated before names (plan doc 10 §6.1).
  entry.readonly = true;
  core::MutateGuard ro(locks, exports, entry, cred);
  auto verdict = ro.precheck({"with/slash"});
  EXPECT_TRUE(verdict.kind == core::MutateGuard::Verdict::kReadonly);
}

TEST(CoreMutate, ChangeSampleDefaults) {
  core::ChangeSample sample;
  EXPECT_EQ(sample.change_before(), 0u);
  EXPECT_EQ(sample.change_after(), 0u);
  backend::Attr attr;
  attr.change = 42;
  sample.before = attr;
  attr.change = 43;
  sample.after = attr;
  EXPECT_EQ(sample.change_before(), 42u);
  EXPECT_EQ(sample.change_after(), 43u);
}
