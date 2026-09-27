#pragma once

// UTF-8 validation, decoding and conversion to and from UTF-16. CFW text is
// UTF-8; UTF-16 is only for the OS boundary (Win32 wide APIs).
//
// "Valid" means well-formed per Unicode §3.9: no overlong forms, no encoded
// surrogates, nothing above U+10FFFF, no truncated sequences.
//
// Threads: any (pure functions). Allocates: only the converting functions,
// for their result. On failure: Result with ErrorCode::ParseError and the byte
// or unit offset of the first bad sequence.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "cfw/core/Result.h"
#include "cfw/core/String.h"

namespace cfw {

inline constexpr char32_t kReplacementCharacter = U'�';

// One decoded code point. On an invalid sequence, `valid` is false,
// `codepoint` is U+FFFD and `length` is the number of bytes to skip (at least
// 1), so a loop that always advances by `length` terminates and resynchronises.
struct Utf8Char {
    char32_t codepoint = kReplacementCharacter;
    std::uint8_t length = 1;
    bool valid = false;
};

// Decodes the code point starting at byte `offset`. `offset` must be less than
// text.size().
[[nodiscard]] Utf8Char decodeUtf8At(StringView text, std::size_t offset) noexcept;

// True if `text` is entirely well-formed UTF-8.
[[nodiscard]] bool isValidUtf8(StringView text) noexcept;

// Offset of the first invalid byte, or text.size() if the text is valid.
[[nodiscard]] std::size_t findInvalidUtf8(StringView text) noexcept;

// Appends the UTF-8 encoding of `codepoint` to `out`. Surrogates and values
// above U+10FFFF append U+FFFD instead.
void appendUtf8(String &out, char32_t codepoint);

// Number of code points (not glyphs, not user-perceived characters). Invalid
// sequences count once per resynchronisation step.
[[nodiscard]] std::size_t countCodepoints(StringView text) noexcept;

// Orders two UTF-8 strings as their UTF-16 encodings would compare unit by
// unit. This differs from byte (code point) order only when a character above
// U+FFFF meets one in U+E000..U+FFFF: UTF-16 sorts the former first. Needed to
// reproduce the key order of JSON written by UTF-16-based tools (the scene
// files the Qt build wrote). Returns <0, 0 or >0.
[[nodiscard]] int compareUtf16Order(StringView a, StringView b) noexcept;

// Strict conversions: fail on the first ill-formed sequence.
[[nodiscard]] Result<std::u16string> utf8ToUtf16(StringView text);
[[nodiscard]] Result<String> utf16ToUtf8(std::u16string_view text);

// Lossy conversions: ill-formed sequences become U+FFFD. Windows file names may
// contain unpaired surrogates, so display code uses this form.
[[nodiscard]] String utf16ToUtf8Lossy(std::u16string_view text);
[[nodiscard]] String sanitizeUtf8(StringView text);

} // namespace cfw
