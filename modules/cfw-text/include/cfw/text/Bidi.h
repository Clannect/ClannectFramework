#pragma once

// The Unicode Bidirectional Algorithm (UAX #9): embedding levels for mixed
// left-to-right and right-to-left text, and the visual order of a line.
// Complete: explicit embeddings, overrides and isolates (depth 125), weak
// and neutral types, bracket pairs (N0), and the line rules L1/L2. It passes
// the Unicode conformance tests (BidiTest.txt, BidiCharacterTest.txt).
//
// Levels are even for left-to-right, odd for right-to-left. Characters that
// the algorithm removes (X9: embedding controls and Boundary_Neutral) get the
// level of the character before them, so every character has a level.
//
// Threads: stateless. Allocates: the output vectors and scratch storage.

#include <cstdint>
#include <optional>
#include <vector>

#include "cfw/core/Span.h"
#include "cfw/text/UnicodeEnums.h"

namespace cfw {

enum class TextDirection : std::uint8_t { LeftToRight, RightToLeft };

struct BidiParagraph {
    std::uint8_t level = 0;            // the paragraph embedding level (0 or 1)
    std::vector<std::uint8_t> levels;  // per character, before the line rules
};

// Resolves the levels of one paragraph of text (paragraph separators end it:
// split text into paragraphs first). `direction` forces the paragraph level;
// without one it comes from the first strong character (P2, P3).
void resolveBidi(Span<const char32_t> text, std::optional<TextDirection> direction, BidiParagraph &out);

// The same from bidi classes, for callers that already have them. `text`
// (same length, or empty) is only used to find bracket pairs (N0).
void resolveBidi(Span<const unicode::BidiClass> classes, Span<const char32_t> text,
                 std::optional<TextDirection> direction, BidiParagraph &out);

// L1 for one line: [begin, end) of the paragraph. Resets segment and
// paragraph separators, and whitespace before them or at the line's end, to
// the paragraph level. `levels` is the paragraph's (modified in place).
void applyBidiLineRules(Span<const unicode::BidiClass> classes, std::uint8_t paragraphLevel,
                        Span<std::uint8_t> levels, std::size_t begin, std::size_t end);

// L2: the visual order of a line, as indices into `levels` (display order,
// left to right).
void bidiVisualOrder(Span<const std::uint8_t> levels, std::vector<std::uint32_t> &order);

} // namespace cfw
