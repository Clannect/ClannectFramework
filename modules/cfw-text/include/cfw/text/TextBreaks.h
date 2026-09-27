#pragma once

// Where text may be divided: extended grapheme clusters (UAX #29), what a
// caret steps over and a backspace deletes, and line break opportunities
// (UAX #14, the default algorithm). Both run on code points and pass the
// Unicode conformance tests (GraphemeBreakTest.txt, LineBreakTest.txt).
//
// Line breaking does not use dictionaries: text in scripts written without
// spaces (Thai, Lao, Khmer, Myanmar: class SA) breaks only at spaces and
// punctuation. That is a known limitation, recorded in docs/decisions/0015.
//
// Threads: stateless. Allocates: the output vectors (reused if passed again)
// and, for line breaking, a scratch vector.

#include <cstdint>
#include <vector>

#include "cfw/core/Span.h"

namespace cfw {

// out[i] is 1 where a grapheme cluster starts at text[i]; out has
// text.size() + 1 entries, and out[text.size()] is 1 (the end).
void graphemeBoundaries(Span<const char32_t> text, std::vector<std::uint8_t> &out);

enum class LineBreak : std::uint8_t {
    None,      // the line may not break here
    Allowed,   // a break opportunity
    Mandatory, // after a hard line break (LF, CR LF, NEL, LS, PS, ...)
};

// out[i] is the opportunity before text[i]; out has text.size() + 1 entries.
// out[0] is None and out[text.size()] is Mandatory (end of text).
void lineBreaks(Span<const char32_t> text, std::vector<LineBreak> &out);

} // namespace cfw
