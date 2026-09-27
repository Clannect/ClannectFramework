#pragma once

// OpenType text shaping: a run of characters in one font, script and
// direction becomes positioned glyphs. CFW's own implementation of what
// HarfBuzz does (decision 0015), tested glyph for glyph against it:
// font-guided normalisation (compose what the font has, decompose what it
// does not, reorder marks), GSUB substitutions (ligatures, contextual forms,
// Arabic joining), GPOS positioning (kerning, mark attachment, cursive
// connection), the legacy 'kern' table, and for fonts without layout tables,
// marks placed by glyph boxes and Arabic forms from presentation forms.
//
// Engines: the default one (Latin, Greek, Cyrillic, CJK and the other simple
// scripts), Arabic and Syriac, Hebrew, and Thai and Lao. Not yet: the Indic,
// Khmer, Myanmar, Hangul and Universal Shaping Engine scripts, which shape
// with the default engine (no syllable reordering); see decision 0015.
//
// Positions are in font units; scale by size / unitsPerEm(). Right-to-left
// runs come out in visual order (left to right), with clusters decreasing.
//
// Threads: one Shaper per thread; it caches plans per font and settings.
// Allocates: the output and its buffers (reused across calls), and a plan
// the first time a font, script, direction and feature set is used.

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "cfw/core/Span.h"
#include "cfw/text/Bidi.h"
#include "cfw/text/FontFace.h"
#include "cfw/text/UnicodeEnums.h"

namespace cfw {

struct ShapedGlyph {
    GlyphId glyph = 0;
    std::uint32_t cluster = 0; // index of the first character the glyph belongs to
    std::int32_t xAdvance = 0;
    std::int32_t yAdvance = 0;
    std::int32_t xOffset = 0;
    std::int32_t yOffset = 0;
};

// An OpenType feature switched on (value 1+) or off (0), for characters
// [start, end) or the whole run.
struct FontFeature {
    std::uint32_t tag = 0;
    std::uint32_t value = 1;
    std::uint32_t start = 0;
    std::uint32_t end = 0xFFFFFFFFu;
};

struct ShapeOptions {
    // The script, or Unknown to take the first specific script in the text.
    unicode::Script script = unicode::Script::Unknown;
    // Default: the script's own direction.
    std::optional<TextDirection> direction;
    // An OpenType language system tag, 0 for the default.
    std::uint32_t language = 0;
    Span<const FontFeature> features;
};

class Shaper {
public:
    Shaper();
    ~Shaper();
    Shaper(const Shaper &) = delete;
    Shaper &operator=(const Shaper &) = delete;

    void shape(const FontFace &face, Span<const char32_t> text, const ShapeOptions &options,
               std::vector<ShapedGlyph> &out);

    // The script's natural direction.
    [[nodiscard]] static TextDirection scriptDirection(unicode::Script script) noexcept;

    struct Plan; // Shaper.cpp

private:
    const Plan &plan(const FontFace &face, unicode::Script script, TextDirection direction, std::uint32_t language,
                     Span<const FontFeature> features);

    std::vector<std::unique_ptr<Plan>> m_plans;
    struct Scratch;
    std::unique_ptr<Scratch> m_scratch;
};

} // namespace cfw
