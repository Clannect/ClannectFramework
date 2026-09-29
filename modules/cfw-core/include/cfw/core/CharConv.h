#pragma once

// Floating-point std::from_chars where the standard library has it, and a
// strict equivalent where it does not (Apple's libc++ before LLVM 20).
// Same contract as std::from_chars with chars_format::general: no leading
// whitespace or '+', no hex, decimal or scientific digits, inf and nan;
// out of range (overflow, or underflow to zero) leaves `value` untouched
// and reports result_out_of_range. Integers need no wrapper:
// std::from_chars for them is everywhere.
//
// Threads: pure functions.

#include <charconv>

namespace cfw {

std::from_chars_result fromChars(const char *first, const char *last, double &value) noexcept;
std::from_chars_result fromChars(const char *first, const char *last, float &value) noexcept;

namespace detail {
// The portable implementation (strtod on the longest valid prefix, in the
// C locale). Always built, so it is tested against std::from_chars.
std::from_chars_result fromCharsFallback(const char *first, const char *last, double &value) noexcept;
std::from_chars_result fromCharsFallback(const char *first, const char *last, float &value) noexcept;
} // namespace detail

} // namespace cfw
