#pragma once

// One face of an OpenType font file (.ttf, .otf, or one face of a .ttc/.otc
// collection): character mapping, metrics, and glyph outlines from TrueType
// (`glyf`) or CFF (`CFF `) data. CFW's own parser (decision 0015): every read
// is bounds-checked, so hostile files fail to load or yield empty glyphs;
// they never read outside the data.
//
// Units: everything is in font units (unitsPerEm per em) with y pointing up,
// as fonts store it. Scale by size / unitsPerEm() and flip y to draw.
//
// Not supported yet: variable fonts (fvar/gvar, CFF2), vertical metrics,
// bitmap and colour glyphs (sbix, CBDT, COLR), WOFF. Such faces load; the
// missing data is simply not used.
//
// Threads: immutable after load; share freely. Allocates: at load (a table
// directory and a few offsets), and in glyphOutline() only for the output
// path and CFF's operand storage.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "cfw/core/PainterPath.h"
#include "cfw/core/Rect.h"
#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"

namespace cfw {

using GlyphId = std::uint16_t;

class FontFace {
public:
    // The font data is shared: faces from one collection file share it.
    using Data = std::shared_ptr<const std::vector<std::byte>>;

    // Number of faces in the file: 1 for a plain font, n for a collection.
    [[nodiscard]] static Result<std::uint32_t> faceCount(Span<const std::byte> data);
    [[nodiscard]] static Result<std::shared_ptr<const FontFace>> load(Data data, std::uint32_t index = 0);

    // A face's names and style, read from its head, OS/2 and name tables
    // alone: cheap enough to list every installed font. Fails where load()
    // would fail for a missing or bad head/maxp/hhea/hmtx.
    struct Description {
        String family;
        String style;
        int weight = 400;
        bool italic = false;
    };
    [[nodiscard]] static Result<Description> describe(Span<const std::byte> data, std::uint32_t index = 0);

    // A 4-byte table tag, e.g. tag("GSUB").
    [[nodiscard]] static constexpr std::uint32_t tag(const char (&t)[5]) noexcept {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(t[0])) << 24 |
               static_cast<std::uint32_t>(static_cast<unsigned char>(t[1])) << 16 |
               static_cast<std::uint32_t>(static_cast<unsigned char>(t[2])) << 8 |
               static_cast<std::uint32_t>(static_cast<unsigned char>(t[3]));
    }
    // Identifies this face for caches: unique among all faces loaded in the
    // process, never reused (unlike the address).
    [[nodiscard]] std::uint64_t uniqueId() const noexcept { return m_uniqueId; }

    // A table's bytes (empty if the face has none).
    [[nodiscard]] Span<const std::byte> table(std::uint32_t tag) const noexcept;

    // Names (from `name`; typographic family and subfamily when present).
    [[nodiscard]] const String &familyName() const noexcept { return m_family; }
    [[nodiscard]] const String &styleName() const noexcept { return m_style; }
    // OS/2 weight class (400 regular, 700 bold) and italic flag.
    [[nodiscard]] int weight() const noexcept { return m_weight; }
    [[nodiscard]] bool isItalic() const noexcept { return m_italic; }
    [[nodiscard]] bool isFixedPitch() const noexcept { return m_fixedPitch; }

    [[nodiscard]] int unitsPerEm() const noexcept { return m_unitsPerEm; }
    [[nodiscard]] std::uint32_t glyphCount() const noexcept { return m_glyphCount; }

    // Vertical metrics: hhea's, or OS/2's typographic ones when the font
    // asks for them (USE_TYPO_METRICS). Descender is negative.
    struct LineMetrics {
        int ascender = 0;
        int descender = 0;
        int lineGap = 0;
    };
    [[nodiscard]] LineMetrics lineMetrics() const noexcept { return m_line; }
    // OS/2 usWinAscent/usWinDescent (what Windows GDI uses for line height;
    // both positive).
    [[nodiscard]] int winAscent() const noexcept { return m_winAscent; }
    [[nodiscard]] int winDescent() const noexcept { return m_winDescent; }
    [[nodiscard]] int xHeight() const noexcept { return m_xHeight; }
    [[nodiscard]] int capHeight() const noexcept { return m_capHeight; }
    [[nodiscard]] int underlinePosition() const noexcept { return m_underlinePosition; }
    [[nodiscard]] int underlineThickness() const noexcept { return m_underlineThickness; }
    [[nodiscard]] int strikeoutPosition() const noexcept { return m_strikeoutPosition; }
    [[nodiscard]] int strikeoutThickness() const noexcept { return m_strikeoutThickness; }

    // The glyph for a character, 0 (.notdef) if the font has none.
    [[nodiscard]] GlyphId glyphIndex(char32_t c) const noexcept;
    // With a variation selector (cmap format 14): the variant glyph, or
    // glyphIndex(c) if the sequence is default or unknown.
    [[nodiscard]] GlyphId glyphIndex(char32_t c, char32_t selector) const noexcept;
    // The glyph for a variation sequence the font lists (its default glyph
    // for a default sequence), or nothing if the font does not list it.
    [[nodiscard]] std::optional<GlyphId> variationGlyph(char32_t c, char32_t selector) const noexcept;

    [[nodiscard]] int advanceWidth(GlyphId glyph) const noexcept;
    [[nodiscard]] int leftSideBearing(GlyphId glyph) const noexcept;

    // A glyph's ink box in font units, y up: left edge, top, width, and
    // height (negative: downwards), as HarfBuzz reports it (TrueType: the glyf
    // header box placed at the hmtx side bearing; CFF: the outline's control
    // points). All zero for an empty glyph; false if the glyph is missing.
    struct GlyphExtents {
        int xBearing = 0;
        int yBearing = 0;
        int width = 0;
        int height = 0;
    };
    [[nodiscard]] bool glyphExtents(GlyphId glyph, GlyphExtents &out) const;

    // The glyph's outline, replacing `out`. False (and an empty path) for a
    // glyph that does not exist or whose data is broken; true and an empty
    // path for an empty glyph (a space).
    bool glyphOutline(GlyphId glyph, PainterPath &out) const;

    // True if the outlines are CFF (cubic), false for TrueType (quadratic).
    [[nodiscard]] bool hasCffOutlines() const noexcept { return m_cff != nullptr; }

    FontFace(const FontFace &) = delete;
    FontFace &operator=(const FontFace &) = delete;
    ~FontFace();

    struct Cff; // parsed CFF structures (FontFace.cpp)

private:
    FontFace() = default;
    Result<void> parse(std::uint32_t index);
    bool trueTypeOutline(GlyphId glyph, PainterPath &out, int depth, std::vector<Vec2> *points) const;

    struct TableEntry {
        std::uint32_t tag;
        std::uint32_t offset;
        std::uint32_t length;
    };

    Data m_data;
    std::uint64_t m_uniqueId = 0;
    std::vector<TableEntry> m_tables;
    String m_family;
    String m_style;
    int m_weight = 400;
    bool m_italic = false;
    bool m_fixedPitch = false;
    int m_unitsPerEm = 1000;
    std::uint32_t m_glyphCount = 0;
    LineMetrics m_line;
    int m_winAscent = 0;
    int m_winDescent = 0;
    int m_xHeight = 0;
    int m_capHeight = 0;
    int m_underlinePosition = 0;
    int m_underlineThickness = 0;
    int m_strikeoutPosition = 0;
    int m_strikeoutThickness = 0;
    // cmap: the chosen Unicode subtable and the variation subtable (offsets
    // into the file; 0 if none).
    std::uint32_t m_cmap = 0;
    std::uint32_t m_cmapEnd = 0;
    std::uint16_t m_cmapFormat = 0;
    bool m_cmapSymbol = false;
    std::uint32_t m_cmapVariations = 0;
    std::uint32_t m_cmapVariationsEnd = 0;
    // hmtx
    std::uint32_t m_hmtx = 0;
    std::uint32_t m_hmtxEnd = 0;
    std::uint32_t m_hMetricCount = 0;
    // glyf/loca
    std::uint32_t m_glyf = 0;
    std::uint32_t m_glyfEnd = 0;
    std::uint32_t m_loca = 0;
    std::uint32_t m_locaEnd = 0;
    bool m_longLoca = false;
    std::unique_ptr<Cff> m_cff;
};

} // namespace cfw
