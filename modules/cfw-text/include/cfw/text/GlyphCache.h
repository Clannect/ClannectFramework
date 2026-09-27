#pragma once

// Glyph images for drawing text: each glyph rasterised once per font, pixel
// size and sub-pixel offset into an 8-bit coverage mask, packed into atlas
// pages (the CPU backend reads them directly; a GPU backend uploads pages
// as textures, re-uploading when a page's generation changes).
//
// Masks are unhinted, anti-aliased coverage from cfw-image's Rasterizer
// (the technique of FreeType's "gray" rasteriser), compared with FreeType's
// unhinted rendering in GlyphCacheTest. Horizontal positions are quantised
// to quarter pixels (sub-pixel positioning); vertical ones to whole pixels.
//
// Threads: one cache per thread (or external locking). Allocates: only on a
// miss (a new glyph, or a new page).

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "cfw/core/Span.h"
#include "cfw/image/Rasterizer.h"
#include "cfw/image/RectPacker.h"
#include "cfw/text/FontFace.h"

namespace cfw {

// Where a glyph's mask is: page and rectangle in the atlas, and the offset
// of its top-left pixel from the pen position (y down). An empty glyph (a
// space) has zero width and height.
struct GlyphMask {
    std::uint16_t page = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::int16_t left = 0;
    std::int16_t top = 0;
};

class GlyphCache {
public:
    static constexpr int kSubpixelSteps = 4;

    explicit GlyphCache(int pageSize = 1024);

    // The mask of `glyph` at `pixelSize` (pixels per em), for a pen whose
    // fractional x position is `subpixelX` (0 <= subpixelX < 1; quantised).
    // Nothing for glyphs too large for a page (more than a quarter of it:
    // draw those as paths) or that do not exist.
    [[nodiscard]] const GlyphMask *glyph(const FontFace &face, GlyphId glyph, float pixelSize, float subpixelX);

    // Atlas pages: pageSize x pageSize bytes of coverage, rows top to bottom.
    [[nodiscard]] int pageSize() const noexcept { return m_pageSize; }
    [[nodiscard]] std::size_t pageCount() const noexcept { return m_pages.size(); }
    [[nodiscard]] Span<const std::uint8_t> page(std::size_t index) const noexcept { return m_pages[index]->pixels; }
    // Changes whenever glyphs are added to the page (for texture uploads).
    [[nodiscard]] std::uint64_t pageGeneration(std::size_t index) const noexcept { return m_pages[index]->generation; }

    // Forgets every glyph and page.
    void clear();

    [[nodiscard]] std::uint64_t hits() const noexcept { return m_hits; }
    [[nodiscard]] std::uint64_t misses() const noexcept { return m_misses; }
    [[nodiscard]] std::size_t glyphCount() const noexcept { return m_glyphs.size(); }

    // Rasterises one glyph into `out` (width x height bytes), as glyph()
    // does, without caching: for tests and one-off rendering. Returns false
    // for a glyph that does not exist.
    static bool render(const FontFace &face, GlyphId glyph, float pixelSize, float subpixelX, GlyphMask &box,
                       std::vector<std::uint8_t> &out);

private:
    struct Key {
        std::uint64_t face;
        std::uint32_t glyph;
        std::uint32_t size; // pixel size in 1/64 px, and the sub-pixel step in the low bits
        bool operator==(const Key &) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key &k) const noexcept {
            std::uint64_t h = k.face * 0x9E3779B97F4A7C15ull;
            h ^= (static_cast<std::uint64_t>(k.glyph) << 32 | k.size) + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
            return static_cast<std::size_t>(h ^ (h >> 29));
        }
    };
    struct Page {
        std::vector<std::uint8_t> pixels;
        RectPacker packer;
        std::uint64_t generation = 0;
        Page(int size) : pixels(static_cast<std::size_t>(size) * static_cast<std::size_t>(size)), packer(size, size, 1) {}
    };

    int m_pageSize;
    std::vector<std::unique_ptr<Page>> m_pages;
    std::unordered_map<Key, GlyphMask, KeyHash> m_glyphs;
    std::vector<std::uint8_t> m_scratch;
    std::uint64_t m_hits = 0;
    std::uint64_t m_misses = 0;
};

} // namespace cfw
