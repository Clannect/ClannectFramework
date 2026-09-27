#pragma once

// String utilities: trimming, splitting, joining, ASCII case folding, and
// locale-independent number parsing and formatting.
//
// Case-insensitive comparison folds ASCII only. Full Unicode case folding
// needs the Unicode tables that cfw-text will carry; until then non-ASCII bytes
// compare exactly. Every caller today compares identifiers and class names,
// which are ASCII.
//
// Threads: any (pure functions). Allocates: only functions returning String or
// std::vector. On failure: parse functions return ErrorCode::ParseError or
// ErrorCode::OutOfRange.

#include <cstdint>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/String.h"

namespace cfw {

// ASCII whitespace: space, \t, \n, \v, \f, \r.
[[nodiscard]] constexpr bool isAsciiSpace(char c) noexcept {
    return c == ' ' || (c >= '\t' && c <= '\r');
}
[[nodiscard]] constexpr char toLowerAscii(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}
[[nodiscard]] constexpr char toUpperAscii(char c) noexcept {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

[[nodiscard]] StringView trim(StringView text) noexcept;
[[nodiscard]] StringView trimStart(StringView text) noexcept;
[[nodiscard]] StringView trimEnd(StringView text) noexcept;

enum class SplitMode { KeepEmpty, SkipEmpty };

// Views into `text`; they are only valid while `text` is.
[[nodiscard]] std::vector<StringView> split(StringView text, char separator,
                                            SplitMode mode = SplitMode::KeepEmpty);
// `separator` must not be empty.
[[nodiscard]] std::vector<StringView> split(StringView text, StringView separator,
                                            SplitMode mode = SplitMode::KeepEmpty);

[[nodiscard]] String join(const std::vector<StringView> &parts, StringView separator);
[[nodiscard]] String join(const std::vector<String> &parts, StringView separator);

[[nodiscard]] constexpr bool contains(StringView text, StringView needle) noexcept {
    return text.find(needle) != StringView::npos;
}

[[nodiscard]] String toLowerAscii(StringView text);
[[nodiscard]] String toUpperAscii(StringView text);

[[nodiscard]] bool equalsIgnoreCase(StringView a, StringView b) noexcept;
// <0, 0, >0 like strcmp, folding ASCII letters.
[[nodiscard]] int compareIgnoreCase(StringView a, StringView b) noexcept;
[[nodiscard]] bool startsWithIgnoreCase(StringView text, StringView prefix) noexcept;

// Strict, locale-independent parsing. The whole of `text` must be the number:
// no surrounding whitespace, no '+' sign, no thousands separators. Integers
// are decimal. parseDouble accepts decimal and exponent forms and rejects
// "inf" and "nan": a non-finite number in scene or network data is always a
// bug upstream.
[[nodiscard]] Result<std::int64_t> parseInt(StringView text);
[[nodiscard]] Result<double> parseDouble(StringView text);

// Locale-independent formatting. formatDouble gives the shortest text that
// parses back to exactly the same double.
[[nodiscard]] String formatInt(std::int64_t value);
[[nodiscard]] String formatDouble(double value);

} // namespace cfw
