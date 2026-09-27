#pragma once

// Paragraph layout: text becomes positioned glyphs in lines.
//
//   - Itemising: hard line breaks split paragraphs; within one, runs of one
//     bidi level (UAX #9), script and font. Characters the font lacks come
//     from a FontDatabase fallback, grapheme by grapheme.
//   - Shaping through a ShapeCache (per run text, face, script, direction
//     and features; results are in font units, so they serve every size).
//   - Wrapping at line-break opportunities (UAX #14) to a width, breaking
//     inside a word only when it alone is wider; spaces at a line's end
//     hang. Optional ellipsis, line limit and alignment.
//   - Lines in visual order (runs reordered by level); carets at grapheme
//     boundaries, and hit-testing a point to a caret.
//
// Positions are pixels, y down, relative to the layout's top-left; glyphs
// are on their line's baseline. Text indices are code points (see
// utf8Offset() for UTF-8 text).
//
// Laying out the same text, style and options again returns at once, so a
// static label can be laid out every frame for nothing.
//
// Threads: one layout per thread. Allocates: its buffers, reused across
// layouts.

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "cfw/core/Rect.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"
#include "cfw/text/Bidi.h"
#include "cfw/text/FontFace.h"
#include "cfw/text/Shaper.h"

namespace cfw {

class FontDatabase;

// Shaping results, cached. Positions are in font units.
class ShapeCache {
public:
    explicit ShapeCache(std::size_t capacity = 4096);
    ~ShapeCache();
    ShapeCache(const ShapeCache &) = delete;
    ShapeCache &operator=(const ShapeCache &) = delete;

    [[nodiscard]] Span<const ShapedGlyph> shape(const FontFace &face, Span<const char32_t> text, const ShapeOptions &options);
    void clear();
    [[nodiscard]] std::uint64_t hits() const noexcept { return m_hits; }
    [[nodiscard]] std::uint64_t misses() const noexcept { return m_misses; }

private:
    struct Entry;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::uint64_t m_hits = 0;
    std::uint64_t m_misses = 0;
};

enum class TextAlign : std::uint8_t { Start, End, Left, Right, Center };

struct TextStyle {
    std::shared_ptr<const FontFace> font;
    float pixelSize = 14.0f;
    Span<const FontFeature> features; // must outlive the layout call
    FontDatabase *fallback = nullptr;  // for characters the font lacks
};

struct TextLayoutOptions {
    float maxWidth = std::numeric_limits<float>::infinity();
    bool wrap = true;             // wrap at maxWidth (else one line per paragraph)
    bool elide = false;           // end the last line with "…" when text is cut
    int maxLines = 0;             // 0: no limit
    TextAlign align = TextAlign::Start;
    std::optional<TextDirection> direction; // paragraph direction; default: the first strong character's
    float lineSpacing = 1.0f;     // multiple of the font's line height

    bool operator==(const TextLayoutOptions &) const = default;
};

class TextLayout {
public:
    TextLayout();
    ~TextLayout();
    TextLayout(TextLayout &&) noexcept;
    TextLayout &operator=(TextLayout &&) noexcept;

    void setText(Span<const char32_t> text);
    void setText(StringView utf8); // invalid sequences become U+FFFD
    [[nodiscard]] Span<const char32_t> text() const noexcept { return m_text; }
    // Byte offset in the UTF-8 text given to setText() of code point `index`.
    [[nodiscard]] std::size_t utf8Offset(std::size_t index) const noexcept;

    // Lays the text out. `cache` defaults to one per thread.
    void layout(const TextStyle &style, const TextLayoutOptions &options = {}, ShapeCache *cache = nullptr);

    struct Glyph {
        GlyphId glyph;
        std::uint16_t font;     // index into fonts()
        float x;                // pen position (baseline origin)
        float y;
        std::uint32_t cluster;  // the first code point it shows
    };
    struct Line {
        std::uint32_t start;    // text range [start, end), the line break included
        std::uint32_t end;
        std::uint32_t firstGlyph;
        std::uint32_t glyphCount;
        float x;                // left edge of the ink run after alignment
        float width;            // advance width, hanging spaces excluded
        float top;
        float baseline;
        float ascent;
        float descent;
        bool rtl;               // paragraph direction
    };
    [[nodiscard]] Span<const Glyph> glyphs() const noexcept { return m_glyphs; }
    [[nodiscard]] Span<const Line> lines() const noexcept { return m_lines; }
    [[nodiscard]] Span<const std::shared_ptr<const FontFace>> fonts() const noexcept { return m_fonts; }
    [[nodiscard]] float pixelSize() const noexcept { return m_pixelSize; }
    // Width (widest line) and height (all lines).
    [[nodiscard]] Vec2 size() const noexcept { return m_size; }
    [[nodiscard]] bool elided() const noexcept { return m_elided; }

    // The caret before code point `index` (clamped; moved to a grapheme
    // boundary): x, and the line's top and bottom.
    struct Caret {
        float x;
        float top;
        float bottom;
        std::size_t line;
    };
    [[nodiscard]] Caret caret(std::size_t index) const;
    // The caret index nearest to `point`.
    [[nodiscard]] std::size_t hitTest(Vec2 point) const;
    // Grapheme boundaries after and before `index` (for arrow keys).
    [[nodiscard]] std::size_t nextCaret(std::size_t index) const;
    [[nodiscard]] std::size_t previousCaret(std::size_t index) const;

private:
    struct Scratch;
    void build(const TextStyle &style, const TextLayoutOptions &options, ShapeCache &cache);
    [[nodiscard]] float caretX(const Line &line, std::size_t index) const;

    std::vector<char32_t> m_text;
    std::vector<std::uint32_t> m_utf8;       // code point -> byte offset (when set from UTF-8)
    std::vector<std::uint8_t> m_graphemes;   // boundaries
    std::vector<Glyph> m_glyphs;
    std::vector<Line> m_lines;
    std::vector<std::shared_ptr<const FontFace>> m_fonts;
    // Per code point: x of its leading and trailing edges on its line, for carets.
    std::vector<float> m_edgeLeft;
    std::vector<float> m_edgeRight;
    std::vector<std::uint8_t> m_rtlChar;
    std::vector<std::uint32_t> m_lineOf;
    float m_pixelSize = 0;
    Vec2 m_size{};
    bool m_elided = false;
    // What the last layout was made from.
    bool m_dirty = true;
    std::shared_ptr<const FontFace> m_lastFont;
    float m_lastSize = 0;
    std::vector<FontFeature> m_lastFeatures;
    FontDatabase *m_lastFallback = nullptr;
    TextLayoutOptions m_lastOptions;
    std::unique_ptr<Scratch> m_scratch;
};

} // namespace cfw
