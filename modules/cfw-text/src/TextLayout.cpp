#include "cfw/text/TextLayout.h"

#include <algorithm>
#include <cmath>
#include <list>

#include "cfw/core/Utf8.h"
#include "cfw/text/FontDatabase.h"
#include "cfw/text/TextBreaks.h"
#include "cfw/text/Unicode.h"

namespace cfw {

using unicode::GeneralCategory;
using unicode::Script;

// ---- ShapeCache ----

struct ShapeCache::Entry {
    std::uint64_t face;
    Script script;
    TextDirection direction;
    std::uint32_t language;
    std::vector<FontFeature> features;
    std::u32string text;
    std::vector<ShapedGlyph> glyphs;
    std::list<std::uint64_t>::iterator lru;
};

struct ShapeCache::Impl {
    std::size_t capacity;
    Shaper shaper;
    std::unordered_multimap<std::uint64_t, Entry> entries;
    std::list<std::uint64_t> order; // most recent first (hash keys)
    std::vector<ShapedGlyph> scratch;
};

ShapeCache::ShapeCache(std::size_t capacity) : m_impl(std::make_unique<Impl>()) {
    m_impl->capacity = std::max<std::size_t>(capacity, 1);
}
ShapeCache::~ShapeCache() = default;

void ShapeCache::clear() {
    m_impl->entries.clear();
    m_impl->order.clear();
}

Span<const ShapedGlyph> ShapeCache::shape(const FontFace &face, Span<const char32_t> text, const ShapeOptions &options) {
    Impl &m = *m_impl;
    std::uint64_t h = face.uniqueId() * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint64_t>(options.script) << 8 ^
                      (options.direction ? static_cast<std::uint64_t>(*options.direction) + 1 : 0) ^
                      static_cast<std::uint64_t>(options.language) << 20;
    for (const char32_t c : text) {
        h = (h ^ c) * 0x100000001B3ull;
    }
    for (const FontFeature &f : options.features) {
        h = (h ^ f.tag ^ static_cast<std::uint64_t>(f.value) << 32 ^ f.start ^ static_cast<std::uint64_t>(f.end) << 17) * 0x100000001B3ull;
    }
    const auto sameFeatures = [&](const Entry &e) {
        return e.features.size() == options.features.size() &&
               std::equal(e.features.begin(), e.features.end(), options.features.begin(), [](const FontFeature &a, const FontFeature &b) {
                   return a.tag == b.tag && a.value == b.value && a.start == b.start && a.end == b.end;
               });
    };
    const TextDirection dir = options.direction.value_or(TextDirection::LeftToRight);
    auto [lo, hi] = m.entries.equal_range(h);
    for (auto it = lo; it != hi; ++it) {
        Entry &e = it->second;
        if (e.face == face.uniqueId() && e.script == options.script && e.direction == dir && e.language == options.language &&
            e.text.size() == text.size() && std::equal(text.begin(), text.end(), e.text.begin()) && sameFeatures(e)) {
            ++m_hits;
            m.order.splice(m.order.begin(), m.order, e.lru);
            return e.glyphs;
        }
    }
    ++m_misses;
    if (m.entries.size() >= m.capacity) {
        // Evict the least recently used entry.
        const auto oldest = std::prev(m.order.end());
        auto [a, b] = m.entries.equal_range(*oldest);
        for (auto it = a; it != b; ++it) {
            if (it->second.lru == oldest) {
                m.entries.erase(it);
                break;
            }
        }
        m.order.pop_back();
    }
    ShapeOptions o = options;
    o.direction = dir;
    m.shaper.shape(face, text, o, m.scratch);
    m.order.push_front(h);
    Entry e{face.uniqueId(), options.script, dir, options.language,
            std::vector<FontFeature>(options.features.begin(), options.features.end()),
            std::u32string(text.begin(), text.end()), m.scratch, m.order.begin()};
    return m.entries.emplace(h, std::move(e))->second.glyphs;
}

// ---- TextLayout ----

namespace {

bool isHangingSpace(char32_t c) {
    return c == U' ' || c == U'\t' || c == 0x3000 || (c != 0xA0 && c != 0x202F && c != 0x2007 &&
                                                       unicode::generalCategory(c) == GeneralCategory::Zs);
}

bool isHardBreak(char32_t c) { return c == U'\n' || c == U'\r' || c == 0x0B || c == 0x0C || c == 0x85 || c == 0x2028 || c == 0x2029; }

// Characters that need no glyph from the font (for choosing fallback fonts).
bool needsNoGlyph(char32_t c) {
    const GeneralCategory gc = unicode::generalCategory(c);
    return gc == GeneralCategory::Cc || gc == GeneralCategory::Cf || gc == GeneralCategory::Zl || gc == GeneralCategory::Zp ||
           (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF) || (c >= 0x180B && c <= 0x180F);
}

float fontScale(const FontFace &face, float pixelSize) { return pixelSize / static_cast<float>(face.unitsPerEm()); }

thread_local ShapeCache t_defaultCache;

} // namespace

struct TextLayout::Scratch {
    struct Item {
        std::uint32_t start;
        std::uint32_t end;
        std::uint8_t level;
        Script script;
        std::uint16_t font;
    };
    struct ItemGlyph {
        GlyphId glyph;
        std::uint32_t cluster;
        std::uint32_t clusterEnd; // the next cluster in logical order (or the item's end)
        float advance;
        float dx;
        float dy;
    };
    std::vector<LineBreak> breaks;
    BidiParagraph bidi;
    std::vector<std::uint8_t> levels;   // per code point (whole text)
    std::vector<std::uint16_t> fontOf;  // per code point
    std::vector<Script> scripts;        // per code point
    std::vector<float> advance;         // per code point, pixels
    std::vector<Item> items;
    std::vector<std::uint32_t> itemGlyphStart; // per item, into glyphs; one extra at the end
    std::vector<ItemGlyph> glyphs;
    std::vector<char32_t> shapingText;
    std::vector<std::uint32_t> clusters;
    std::vector<std::uint8_t> sliceLevels;
    std::vector<std::uint32_t> order;
    struct Slice {
        std::uint32_t item;
        std::uint32_t start;
        std::uint32_t end;
    };
    std::vector<Slice> slices;
};

TextLayout::TextLayout() : m_scratch(std::make_unique<Scratch>()) {}
TextLayout::~TextLayout() = default;
TextLayout::TextLayout(TextLayout &&) noexcept = default;
TextLayout &TextLayout::operator=(TextLayout &&) noexcept = default;

void TextLayout::setText(Span<const char32_t> text) {
    if (!m_utf8.empty() || text.size() != m_text.size() || !std::equal(text.begin(), text.end(), m_text.begin())) {
        m_text.assign(text.begin(), text.end());
        m_utf8.clear();
        m_dirty = true;
    }
}

void TextLayout::setText(StringView utf8) {
    std::vector<char32_t> decoded;
    std::vector<std::uint32_t> offsets;
    decoded.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const Utf8Char c = decodeUtf8At(utf8, i);
        decoded.push_back(c.codepoint);
        offsets.push_back(static_cast<std::uint32_t>(i));
        i += c.length;
    }
    offsets.push_back(static_cast<std::uint32_t>(utf8.size()));
    if (decoded != m_text || offsets != m_utf8) {
        m_text = std::move(decoded);
        m_utf8 = std::move(offsets);
        m_dirty = true;
    }
}

std::size_t TextLayout::utf8Offset(std::size_t index) const noexcept {
    index = std::min(index, m_text.size());
    return m_utf8.empty() ? index : m_utf8[index];
}

void TextLayout::layout(const TextStyle &style, const TextLayoutOptions &options, ShapeCache *cache) {
    const bool same = !m_dirty && style.font == m_lastFont && style.pixelSize == m_lastSize && style.fallback == m_lastFallback &&
                      options == m_lastOptions && style.features.size() == m_lastFeatures.size() &&
                      std::equal(style.features.begin(), style.features.end(), m_lastFeatures.begin(),
                                 [](const FontFeature &a, const FontFeature &b) {
                                     return a.tag == b.tag && a.value == b.value && a.start == b.start && a.end == b.end;
                                 });
    if (same) {
        return;
    }
    m_lastFont = style.font;
    m_lastSize = style.pixelSize;
    m_lastFallback = style.fallback;
    m_lastOptions = options;
    m_lastFeatures.assign(style.features.begin(), style.features.end());
    m_dirty = false;
    build(style, options, cache ? *cache : t_defaultCache);
}

void TextLayout::build(const TextStyle &style, const TextLayoutOptions &options, ShapeCache &cache) {
    Scratch &s = *m_scratch;
    m_glyphs.clear();
    m_lines.clear();
    m_fonts.clear();
    m_elided = false;
    m_size = {};
    const std::size_t n = m_text.size();
    m_edgeLeft.assign(n, 0.0f);
    m_edgeRight.assign(n, 0.0f);
    m_rtlChar.assign(n, 0);
    m_lineOf.assign(n, 0);
    m_pixelSize = style.pixelSize > 0 && std::isfinite(style.pixelSize) ? style.pixelSize : 0.0f;
    if (!style.font) {
        return;
    }
    m_fonts.push_back(style.font);
    const float maxWidth = options.maxWidth > 0 ? options.maxWidth : 0.0f;
    const Span<const char32_t> text(m_text.data(), n);
    graphemeBoundaries(text, m_graphemes);
    lineBreaks(text, s.breaks);
    s.levels.assign(n, 0);
    s.fontOf.assign(n, 0);
    s.scripts.assign(n, Script::Common);
    s.advance.assign(n, 0.0f);

    const auto fontIndex = [&](const std::shared_ptr<const FontFace> &f) -> std::uint16_t {
        for (std::size_t i = 0; i < m_fonts.size(); ++i) {
            if (m_fonts[i] == f) {
                return static_cast<std::uint16_t>(i);
            }
        }
        m_fonts.push_back(f);
        return static_cast<std::uint16_t>(m_fonts.size() - 1);
    };
    // Ellipsis glyphs from the primary font: "…", or "..." without it.
    std::vector<Scratch::ItemGlyph> ellipsis;
    float ellipsisWidth = 0;
    if (options.elide) {
        const char32_t one[] = {0x2026};
        const char32_t three[] = {U'.', U'.', U'.'};
        const bool has = style.font->glyphIndex(0x2026) != 0;
        ShapeOptions o;
        o.script = Script::Common;
        o.direction = TextDirection::LeftToRight;
        const float scale = fontScale(*style.font, m_pixelSize);
        for (const ShapedGlyph &g : cache.shape(*style.font, has ? Span<const char32_t>(one) : Span<const char32_t>(three), o)) {
            ellipsis.push_back({g.glyph, 0, 0, static_cast<float>(g.xAdvance) * scale, static_cast<float>(g.xOffset) * scale, static_cast<float>(g.yOffset) * scale});
            ellipsisWidth += static_cast<float>(g.xAdvance) * scale;
        }
    }

    float top = 0;
    int linesLeft = options.maxLines > 0 ? options.maxLines : -1;
    std::size_t p0 = 0;
    bool stop = false;
    for (;;) {
        // A paragraph: [p0, p1) of text, then its hard break [p1, q).
        std::size_t p1 = p0;
        while (p1 < n && !isHardBreak(m_text[p1])) {
            ++p1;
        }
        std::size_t q = p1;
        if (q < n) {
            q += (m_text[q] == U'\r' && q + 1 < n && m_text[q + 1] == U'\n') ? 2u : 1u;
        }
        const Span<const char32_t> para = text.subspan(p0, p1 - p0);
        resolveBidi(para, options.direction, s.bidi);
        const bool rtl = s.bidi.level & 1;
        for (std::size_t i = p0; i < q; ++i) {
            s.levels[i] = i < p1 ? s.bidi.levels[i - p0] : s.bidi.level;
            m_rtlChar[i] = s.levels[i] & 1;
        }
        // Scripts: Common and Inherited take their neighbours' script.
        Script last = Script::Common;
        for (std::size_t i = p0; i < p1; ++i) {
            const Script sc = unicode::script(m_text[i]);
            if (sc != Script::Common && sc != Script::Inherited && sc != Script::Unknown) {
                if (last == Script::Common) {
                    for (std::size_t k = p0; k < i; ++k) {
                        s.scripts[k] = sc;
                    }
                }
                last = sc;
            }
            s.scripts[i] = last;
        }
        // Fonts: per grapheme, the primary font unless it lacks a character.
        for (std::size_t g = p0; g < p1;) {
            std::size_t e = g + 1;
            while (e < p1 && !m_graphemes[e]) {
                ++e;
            }
            std::uint16_t font = 0;
            if (style.fallback) {
                for (std::size_t k = g; k < e; ++k) {
                    if (!needsNoGlyph(m_text[k]) && style.font->glyphIndex(m_text[k]) == 0) {
                        if (std::shared_ptr<const FontFace> f = style.fallback->fallback(m_text[k], style.font.get())) {
                            font = fontIndex(f);
                        }
                        break;
                    }
                }
            }
            for (std::size_t k = g; k < e; ++k) {
                s.fontOf[k] = font;
            }
            g = e;
        }
        // Items, shaped.
        s.items.clear();
        for (std::size_t i = p0; i < p1;) {
            std::size_t e = i + 1;
            while (e < p1 && s.levels[e] == s.levels[i] && s.scripts[e] == s.scripts[i] && s.fontOf[e] == s.fontOf[i]) {
                ++e;
            }
            s.items.push_back({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(e), s.levels[i], s.scripts[i], s.fontOf[i]});
            i = e;
        }
        s.glyphs.clear();
        s.itemGlyphStart.clear();
        for (const Scratch::Item &item : s.items) {
            s.itemGlyphStart.push_back(static_cast<std::uint32_t>(s.glyphs.size()));
            s.shapingText.assign(m_text.begin() + item.start, m_text.begin() + item.end);
            for (char32_t &c : s.shapingText) {
                if (c == U'\t') {
                    c = U' ';
                } else if (c < 0x20 || (c >= 0x7F && c < 0xA0)) {
                    c = 0x200B; // controls draw nothing
                }
            }
            ShapeOptions o;
            o.script = item.script;
            o.direction = item.level & 1 ? TextDirection::RightToLeft : TextDirection::LeftToRight;
            o.features = style.features;
            const FontFace &face = *m_fonts[item.font];
            const float scale = fontScale(face, m_pixelSize);
            const std::size_t first = s.glyphs.size();
            s.clusters.clear();
            for (const ShapedGlyph &g : cache.shape(face, s.shapingText, o)) {
                const std::uint32_t cluster = item.start + g.cluster;
                s.glyphs.push_back({g.glyph, cluster, item.end, static_cast<float>(g.xAdvance) * scale, static_cast<float>(g.xOffset) * scale, static_cast<float>(g.yOffset) * scale});
                s.advance[cluster] += static_cast<float>(g.xAdvance) * scale;
                s.clusters.push_back(cluster);
            }
            std::sort(s.clusters.begin(), s.clusters.end());
            for (std::size_t k = first; k < s.glyphs.size(); ++k) {
                const auto next = std::upper_bound(s.clusters.begin(), s.clusters.end(), s.glyphs[k].cluster);
                if (next != s.clusters.end()) {
                    s.glyphs[k].clusterEnd = *next;
                }
            }
        }
        s.itemGlyphStart.push_back(static_cast<std::uint32_t>(s.glyphs.size()));

        // Lines.
        std::size_t lineStart = p0;
        do {
            std::size_t end = p1;
            float width = 0;
            std::size_t lastBreak = lineStart;
            bool cut = false;
            if (options.wrap && std::isfinite(maxWidth)) {
                for (std::size_t i = lineStart; i < p1; ++i) {
                    if (i > lineStart && s.breaks[i] == LineBreak::Allowed) {
                        lastBreak = i;
                    }
                    if (i > lineStart && !isHangingSpace(m_text[i]) && width + s.advance[i] > maxWidth + 1e-3f) {
                        if (lastBreak > lineStart) {
                            end = lastBreak;
                        } else {
                            // One word wider than the line: break between graphemes.
                            end = i;
                            while (end > lineStart + 1 && !m_graphemes[end]) {
                                --end;
                            }
                            if (!m_graphemes[end]) {
                                end = lineStart + 1;
                                while (end < p1 && !m_graphemes[end]) {
                                    ++end;
                                }
                            }
                        }
                        cut = true;
                        break;
                    }
                    width += s.advance[i];
                }
            }
            if (linesLeft > 0) {
                --linesLeft;
            }
            const bool lastAllowed = linesLeft == 0;
            // Elide: the last allowed line when text continues, or a line
            // wider than the width when not wrapping.
            bool elide = false;
            std::size_t visibleEnd = end;
            if (options.elide && std::isfinite(maxWidth)) {
                const bool more = (lastAllowed && (cut || q < n));
                float w = 0;
                for (std::size_t i = lineStart; i < end; ++i) {
                    w += s.advance[i];
                }
                if (more || (!options.wrap && w > maxWidth)) {
                    elide = true;
                    // Drop graphemes from the end until the ellipsis fits.
                    const std::size_t limit = more ? p1 : end;
                    visibleEnd = limit;
                    float vw = 0;
                    for (std::size_t i = lineStart; i < limit; ++i) {
                        vw += s.advance[i];
                    }
                    while (visibleEnd > lineStart) {
                        float trimmed = vw;
                        for (std::size_t i = visibleEnd; i-- > lineStart && isHangingSpace(m_text[i]);) {
                            trimmed -= s.advance[i];
                        }
                        if (trimmed + ellipsisWidth <= maxWidth + 1e-3f) {
                            break;
                        }
                        std::size_t prev = visibleEnd - 1;
                        while (prev > lineStart && !m_graphemes[prev]) {
                            --prev;
                        }
                        for (std::size_t i = prev; i < visibleEnd; ++i) {
                            vw -= s.advance[i];
                        }
                        visibleEnd = prev;
                    }
                    end = more ? p1 : end;
                }
            }
            // The line's slices of items, in visual order.
            s.slices.clear();
            s.sliceLevels.clear();
            for (std::uint32_t k = 0; k < s.items.size(); ++k) {
                const std::uint32_t a = std::max<std::uint32_t>(s.items[k].start, static_cast<std::uint32_t>(lineStart));
                const std::uint32_t b = std::min<std::uint32_t>(s.items[k].end, static_cast<std::uint32_t>(visibleEnd));
                if (a < b) {
                    s.slices.push_back({k, a, b});
                    s.sliceLevels.push_back(s.items[k].level);
                }
            }
            bidiVisualOrder(s.sliceLevels, s.order);
            const auto lineIndex = static_cast<std::uint32_t>(m_lines.size());
            Line line{};
            line.start = static_cast<std::uint32_t>(lineStart);
            const std::size_t lineEnd = (end >= p1) ? q : end; // the hard break belongs to the paragraph's last line
            line.end = static_cast<std::uint32_t>(lineEnd);
            line.firstGlyph = static_cast<std::uint32_t>(m_glyphs.size());
            line.rtl = rtl;
            float x = 0;
            float ascent = 0;
            float descent = 0;
            float gap = 0;
            const auto addMetrics = [&](const FontFace &f) {
                const float sc = fontScale(f, m_pixelSize);
                ascent = std::max(ascent, static_cast<float>(f.lineMetrics().ascender) * sc);
                descent = std::max(descent, -static_cast<float>(f.lineMetrics().descender) * sc);
                gap = std::max(gap, static_cast<float>(f.lineMetrics().lineGap) * sc);
            };
            addMetrics(*style.font);
            const auto placeEllipsis = [&] {
                for (const Scratch::ItemGlyph &g : ellipsis) {
                    m_glyphs.push_back({g.glyph, 0, x + g.dx, -g.dy, static_cast<std::uint32_t>(visibleEnd)});
                    x += g.advance;
                }
            };
            if (elide && rtl) {
                placeEllipsis();
            }
            for (const std::uint32_t si : s.order) {
                const Scratch::Slice &slice = s.slices[si];
                const Scratch::Item &item = s.items[slice.item];
                addMetrics(*m_fonts[item.font]);
                const bool itemRtl = item.level & 1;
                // Glyphs of the slice, and each cluster's extent for carets.
                const std::uint32_t g0 = s.itemGlyphStart[slice.item];
                const std::uint32_t g1 = s.itemGlyphStart[slice.item + 1];
                for (std::uint32_t gi = g0; gi < g1;) {
                    const std::uint32_t cluster = s.glyphs[gi].cluster;
                    std::uint32_t gj = gi;
                    while (gj < g1 && s.glyphs[gj].cluster == cluster) {
                        ++gj;
                    }
                    if (cluster >= slice.start && cluster < slice.end) {
                        const float clusterX = x;
                        for (std::uint32_t k = gi; k < gj; ++k) {
                            const Scratch::ItemGlyph &g = s.glyphs[k];
                            m_glyphs.push_back({g.glyph, item.font, x + g.dx, -g.dy, g.cluster});
                            x += g.advance;
                        }
                        // The cluster covers code points up to the next cluster in logical order.
                        const std::uint32_t next = std::min(s.glyphs[gi].clusterEnd, slice.end);
                        const float w = x - clusterX;
                        const std::uint32_t count = next - cluster;
                        for (std::uint32_t c = cluster; c < next; ++c) {
                            const float k = static_cast<float>(c - cluster);
                            const float a = clusterX + w * k / static_cast<float>(count);
                            const float b = clusterX + w * (k + 1) / static_cast<float>(count);
                            m_edgeLeft[c] = itemRtl ? clusterX + w - (b - clusterX) : a;
                            m_edgeRight[c] = itemRtl ? clusterX + w - (a - clusterX) : b;
                            m_lineOf[c] = lineIndex;
                        }
                    }
                    gi = gj;
                }
            }
            if (elide && !rtl) {
                placeEllipsis();
            }
            for (std::size_t c = visibleEnd; c < lineEnd; ++c) {
                m_lineOf[c] = lineIndex;
                m_edgeLeft[c] = m_edgeRight[c] = rtl ? 0.0f : x;
            }
            // Width without the spaces hanging at the line's (logical) end.
            float hanging = 0;
            for (std::size_t i = visibleEnd; i-- > lineStart && isHangingSpace(m_text[i]) && !elide;) {
                hanging += s.advance[i];
            }
            line.width = x - hanging;
            line.glyphCount = static_cast<std::uint32_t>(m_glyphs.size()) - line.firstGlyph;
            line.x = rtl ? hanging : 0.0f; // aligned below
            line.ascent = ascent;
            line.descent = descent;
            line.top = top;
            line.baseline = top + ascent;
            for (std::uint32_t k = line.firstGlyph; k < m_glyphs.size(); ++k) {
                m_glyphs[k].y += line.baseline;
            }
            top += (ascent + descent + gap) * options.lineSpacing;
            m_lines.push_back(line);
            m_size.x = std::max(m_size.x, line.width);
            if (elide) {
                m_elided = true;
            }
            if (lastAllowed) {
                stop = true;
                break;
            }
            lineStart = end;
        } while (lineStart < p1);
        if (stop || q == p1) {
            break; // the line limit, or the end of the text (after a final hard break, one empty paragraph more)
        }
        p0 = q;
    }
    // Text cut off by the line limit belongs to the last line's end.
    if (!m_lines.empty()) {
        Line &last = m_lines.back();
        for (std::size_t c = last.end; c < n; ++c) {
            m_lineOf[c] = static_cast<std::uint32_t>(m_lines.size() - 1);
        }
        last.end = static_cast<std::uint32_t>(std::max<std::size_t>(last.end, n));
    }
    m_size.y = top;
    // Alignment.
    const float avail = std::isfinite(maxWidth) ? maxWidth : m_size.x;
    for (Line &line : m_lines) {
        TextAlign a = options.align;
        if (a == TextAlign::Start) {
            a = line.rtl ? TextAlign::Right : TextAlign::Left;
        } else if (a == TextAlign::End) {
            a = line.rtl ? TextAlign::Left : TextAlign::Right;
        }
        float offset = 0;
        if (a == TextAlign::Right) {
            offset = avail - line.width;
        } else if (a == TextAlign::Center) {
            offset = (avail - line.width) / 2;
        }
        const float dx = offset - line.x; // line.x holds the hanging width of right-to-left lines
        line.x = offset;
        for (std::uint32_t k = line.firstGlyph; k < line.firstGlyph + line.glyphCount; ++k) {
            m_glyphs[k].x += dx;
        }
        for (std::size_t c = line.start; c < line.end; ++c) {
            m_edgeLeft[c] += dx;
            m_edgeRight[c] += dx;
        }
    }
}

float TextLayout::caretX(const Line &line, std::size_t index) const {
    if (index < line.end && index < m_text.size() && !isHardBreak(m_text[index])) {
        return m_rtlChar[index] ? m_edgeRight[index] : m_edgeLeft[index];
    }
    // After the line's last character.
    std::size_t last = std::min<std::size_t>(index, line.end);
    while (last > line.start && isHardBreak(m_text[last - 1])) {
        --last;
    }
    if (last == line.start) {
        return line.rtl ? line.x + line.width : line.x;
    }
    return m_rtlChar[last - 1] ? m_edgeLeft[last - 1] : m_edgeRight[last - 1];
}

TextLayout::Caret TextLayout::caret(std::size_t index) const {
    if (m_lines.empty()) {
        return {0, 0, 0, 0};
    }
    index = std::min(index, m_text.size());
    while (index > 0 && index < m_graphemes.size() && !m_graphemes[index]) {
        --index;
    }
    std::size_t li = m_lines.size() - 1;
    for (std::size_t i = 0; i < m_lines.size(); ++i) {
        const Line &l = m_lines[i];
        const bool hardEnd = l.end > l.start && isHardBreak(m_text[l.end - 1]);
        if (index >= l.start && (index < l.end || (index == l.end && !hardEnd && i + 1 == m_lines.size()))) {
            li = i;
            break;
        }
        if (hardEnd && index == l.end && i + 1 == m_lines.size()) {
            li = i; // after a final hard break: no line of its own
        }
    }
    const Line &l = m_lines[li];
    return {caretX(l, index), l.top, l.top + l.ascent + l.descent, li};
}

std::size_t TextLayout::hitTest(Vec2 point) const {
    if (m_lines.empty()) {
        return 0;
    }
    std::size_t li = m_lines.size() - 1;
    for (std::size_t i = 0; i < m_lines.size(); ++i) {
        if (point.y < m_lines[i].top + m_lines[i].ascent + m_lines[i].descent) {
            li = i;
            break;
        }
    }
    const Line &l = m_lines[li];
    std::size_t last = l.end;
    const bool finalLine = li + 1 == m_lines.size();
    if (last > l.start && isHardBreak(m_text[last - 1])) {
        while (last > l.start && isHardBreak(m_text[last - 1])) {
            --last;
        }
    } else if (!finalLine && last > l.start) {
        last = previousCaret(last); // a wrapped line's end is the next line's start
    }
    std::size_t best = l.start;
    float bestDistance = std::numeric_limits<float>::infinity();
    for (std::size_t i = l.start; i <= last; ++i) {
        if (i < m_graphemes.size() && !m_graphemes[i]) {
            continue;
        }
        const float d = std::abs(caretX(l, i) - point.x);
        if (d < bestDistance) {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

std::size_t TextLayout::nextCaret(std::size_t index) const {
    std::size_t i = std::min(index, m_text.size());
    if (i < m_text.size()) {
        ++i;
        while (i < m_text.size() && !m_graphemes[i]) {
            ++i;
        }
    }
    return i;
}

std::size_t TextLayout::previousCaret(std::size_t index) const {
    std::size_t i = std::min(index, m_text.size());
    if (i > 0) {
        --i;
        while (i > 0 && !m_graphemes[i]) {
            --i;
        }
    }
    return i;
}

} // namespace cfw
