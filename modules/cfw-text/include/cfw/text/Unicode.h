#pragma once

// Unicode character properties: what the text algorithms (bidi, line
// breaking, grapheme clusters) and the shaper need. The tables are generated
// from the Unicode Character Database (tools/unicode/generate.py; the version
// is kUnicodeVersion) into a two-stage lookup of about 100 KB.
//
// Code points above U+10FFFF get the properties of an unassigned code point.
//
// Threads: stateless, constant data. Allocates: nothing.

#include <optional>

#include "cfw/core/String.h"
#include "cfw/text/UnicodeEnums.h"

namespace cfw::unicode {

// Every property of one code point, from one table lookup.
struct Properties {
    GeneralCategory generalCategory;
    Script script;
    BidiClass bidiClass;
    LineBreakClass lineBreak;
    GraphemeBreak graphemeBreak;
    IndicConjunctBreak indicConjunctBreak;
    JoiningType joiningType;
    EastAsianWidth eastAsianWidth;
    BracketType bracketType;
    bool extendedPictographic;
    bool defaultIgnorable;
    bool emojiPresentation;
    std::uint8_t combiningClass;
};

[[nodiscard]] Properties properties(char32_t c) noexcept;

[[nodiscard]] inline GeneralCategory generalCategory(char32_t c) noexcept { return properties(c).generalCategory; }
[[nodiscard]] inline Script script(char32_t c) noexcept { return properties(c).script; }
[[nodiscard]] inline BidiClass bidiClass(char32_t c) noexcept { return properties(c).bidiClass; }
[[nodiscard]] inline LineBreakClass lineBreakClass(char32_t c) noexcept { return properties(c).lineBreak; }
[[nodiscard]] inline JoiningType joiningType(char32_t c) noexcept { return properties(c).joiningType; }
[[nodiscard]] inline std::uint8_t combiningClass(char32_t c) noexcept { return properties(c).combiningClass; }

// Marks: Mn, Mc, Me.
[[nodiscard]] bool isMark(GeneralCategory gc) noexcept;

// Bidi_Paired_Bracket: the other bracket of a pair, or `c` itself.
[[nodiscard]] char32_t pairedBracket(char32_t c) noexcept;
// Bidi_Mirroring_Glyph: the mirrored character used in right-to-left text,
// or `c` itself.
[[nodiscard]] char32_t mirrored(char32_t c) noexcept;

// Canonical decomposition, one step: `c` becomes `a` followed by `b` (b is 0
// for a singleton). False if `c` does not decompose. Hangul syllables
// decompose algorithmically.
bool decompose(char32_t c, char32_t &a, char32_t &b) noexcept;
// Canonical composition of a pair into a primary composite (composition
// exclusions never form). False if the pair does not compose.
bool compose(char32_t a, char32_t b, char32_t &composite) noexcept;

// ISO 15924 code ("Latn", "Arab", "Zyyy" for Common).
[[nodiscard]] const char *iso15924(Script script) noexcept;
[[nodiscard]] std::optional<Script> scriptFromIso15924(StringView code) noexcept;

} // namespace cfw::unicode
