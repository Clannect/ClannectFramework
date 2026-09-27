// Unicode text algorithms on arbitrary code points (unassigned, surrogates,
// beyond U+10FFFF, deep embedding and isolate nesting): grapheme clusters,
// line breaking and bidi must stay well-formed. Boundaries start and end the
// text, levels stay within the maximum depth, and the visual order is a
// permutation.

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "FuzzTarget.h"
#include "cfw/text/Bidi.h"
#include "cfw/text/TextBreaks.h"
#include "cfw/text/Unicode.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size < 1) {
        return 0;
    }
    // Mostly interesting characters (controls, bidi formats, marks, scripts),
    // sometimes any value at all.
    static const char32_t kPalette[] = {
        U'a',     U' ',     U'1',     U'.',     U',',     U'(',     U')',     U'[',     U']',     U'-',
        U'\n',    U'\r',    U'\t',    0x05D0,   0x0627,   0x0660,   0x0300,   0x064B,   0x200D,   0x200C,
        0x202A,   0x202B,   0x202C,   0x202D,   0x202E,   0x2066,   0x2067,   0x2068,   0x2069,   0x200E,
        0x200F,   0x061C,   0x00A0,   0x2029,   0x3000,   0x4E00,   0x1F1E6,  0x1F600,  0x1F3FB,  0x0915,
        0x094D,   0x093F,   0xAC00,   0x1100,   0x1161,   0x11A8,   0x0E01,   0x201C,   0x201D,   0x00AB,
        0x25CC,   0x1B05,   0x1B44,   0xFFFD,   0xD800,   0x10FFFF, 0x110000, 0x2329,   0x3009,   0x00AD};
    std::vector<char32_t> text;
    const std::optional<cfw::TextDirection> direction =
        data[0] % 3 == 0 ? std::nullopt
                         : std::optional<cfw::TextDirection>(data[0] % 3 == 1 ? cfw::TextDirection::LeftToRight
                                                                              : cfw::TextDirection::RightToLeft);
    for (std::size_t i = 1; i < size; ++i) {
        if (data[i] < 240) {
            text.push_back(kPalette[data[i] % std::size(kPalette)]);
        } else if (i + 3 < size) {
            text.push_back(static_cast<char32_t>(data[i + 1] | data[i + 2] << 8 | (data[i + 3] & 0x1F) << 16));
            i += 3;
        }
    }
    const std::size_t n = text.size();

    std::vector<std::uint8_t> graphemes;
    cfw::graphemeBoundaries(text, graphemes);
    if (graphemes.size() != n + 1 || (n > 0 && (!graphemes[0] || !graphemes[n]))) {
        std::abort();
    }
    std::vector<cfw::LineBreak> breaks;
    cfw::lineBreaks(text, breaks);
    if (breaks.size() != n + 1 || breaks[n] != cfw::LineBreak::Mandatory || (n > 0 && breaks[0] != cfw::LineBreak::None)) {
        std::abort();
    }

    cfw::BidiParagraph paragraph;
    cfw::resolveBidi(text, direction, paragraph);
    if (paragraph.levels.size() != n || paragraph.level > 1) {
        std::abort();
    }
    std::vector<cfw::unicode::BidiClass> classes(n);
    for (std::size_t i = 0; i < n; ++i) {
        classes[i] = cfw::unicode::bidiClass(text[i]);
        if (paragraph.levels[i] > 126) {
            std::abort();
        }
    }
    cfw::applyBidiLineRules(classes, paragraph.level, paragraph.levels, 0, n);
    std::vector<std::uint32_t> order;
    cfw::bidiVisualOrder(paragraph.levels, order);
    std::vector<std::uint32_t> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (sorted[i] != i) {
            std::abort();
        }
    }
    return 0;
}
