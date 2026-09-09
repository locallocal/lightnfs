#pragma once
// TOML-subset helpers shared by the local config parser (core/config.cpp) and the shared
// export catalog (core/catalog.cpp, plan 12 A2).  Internal to core: nothing outside
// src/core should include this.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/config.hpp"

namespace lnfs::core::detail {

std::string_view trim(std::string_view value);
// Drops a trailing `# comment`, honouring quotes.
std::string strip_comment(std::string_view line);
Result<std::string> string_value(std::string_view value);
Result<uint64_t> uint_value(std::string_view value);
Result<bool> bool_value(std::string_view value);
// A bare number or a quoted "<n>[B|KiB|MiB|GiB]".
Result<uint64_t> size_value(std::string_view value);
Result<std::vector<std::string>> string_array(std::string_view value);
// Duration string in milliseconds: "3s", "1500ms", or a bare number of seconds.
Result<uint64_t> duration_ms_value(std::string_view value);
// Strips trailing slashes (keeps a lone "/").
std::string normalize_path(std::string path);
// A backend subtable value: quoted string, bare bool, or unsigned number (as text).
Result<std::string> backend_value(std::string_view value);

// The inverse direction, for canonical output that `string_value` / `string_array` /
// `backend_value` read back unchanged.
std::string quote(std::string_view value);
std::string quote_array(const std::vector<std::string>& values);
// Bare `true` / `false` / number when that is exactly what `backend_value` would yield,
// otherwise quoted.
std::string backend_value_text(std::string_view value);

// One `[[export]]` block: its keys and the `[export.<backend>]` subtable that may follow
// (the `[[export]]` line itself is the caller's).  Shared by parse_config and
// parse_catalog so the two files use one syntax (design 11, plan 12 A2).  Unknown keys
// are EINVAL; `disabled` (a catalog-only key) is accepted only when a slot is given.
class ExportBlockParser {
 public:
    explicit ExportBlockParser(ExportConfig& exp, bool* disabled = nullptr) : exp_(exp), disabled_(disabled) {}
    // Feeds one trimmed, comment-stripped, non-empty line.  True: the block consumed it (a
    // `key = value` or the `[export.<backend>]` header).  False: the line is some other
    // section header — the block has ended and the caller owns the line.
    Result<bool> line(std::string_view line);

 private:
    ExportConfig& exp_;
    bool* disabled_;
    bool backend_table_ = false;
};

}  // namespace lnfs::core::detail
