#include "cfw/text/GlyphCache.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cfw {

namespace {

// Glyph outlines are small: flatten finely so curves match FreeType's exact
// integration closely.
constexpr float kTolerance = 0.02f;

} // namespace

GlyphCache::GlyphCache(int pageSize) : m_pageSize(std::clamp(pageSize, 64, 8192)) {}

void GlyphCache::clear() {
    m_pages.clear();
    m_glyphs.clear();
}

bool GlyphCache::render(const FontFace &face, GlyphId glyph, float pixelSize, float subpixelX, GlyphMask &box,
                        std::vector<std::uint8_t> &out) {
    thread_local PainterPath path;
    thread_local Rasterizer rasterizer;
    box = {};
    out.clear();
    if (!(pixelSize > 0.0f) || pixelSize > 4096.0f || !face.glyphOutline(glyph, path)) {
        return false;
    }
    if (path.empty()) {
        return true;
    }
    const double scale = static_cast<double>(pixelSize) / face.unitsPerEm();
    const RectF b = path.controlBounds();
    const double sub = std::clamp(static_cast<double>(subpixelX), 0.0, 1.0);
    const double minX = b.x * scale + sub;
    const double maxX = (b.x + b.width) * scale + sub;
    const double minY = -(b.y + b.height) * scale; // y down
    const double maxY = -b.y * scale;
    const double left = std::floor(minX);
    const double top = std::floor(minY);
    const double width = std::ceil(maxX) - left;
    const double height = std::ceil(maxY) - top;
    if (!(width > 0) || !(height > 0) || width > 4096 || height > 4096 || std::abs(left) > 30000 || std::abs(top) > 30000) {
        return true; // empty or absurd: nothing to draw
    }
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    const Transform2D t = Transform2D::scaling(scale, -scale).then(Transform2D::translation(sub - left, -top));
    rasterizer.reset(w, h);
    rasterizer.addPath(path, t, kTolerance);
    out.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    rasterizer.sweep(FillRule::NonZero, [&](int y, int x, Span<const std::uint8_t> coverage) {
        std::memcpy(out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x),
                    coverage.data(), coverage.size());
    });
    box.width = static_cast<std::uint16_t>(w);
    box.height = static_cast<std::uint16_t>(h);
    box.left = static_cast<std::int16_t>(left);
    box.top = static_cast<std::int16_t>(top);
    return true;
}

const GlyphMask *GlyphCache::glyph(const FontFace &face, GlyphId glyph, float pixelSize, float subpixelX) {
    if (!(pixelSize > 0.0f) || pixelSize > 4096.0f) {
        return nullptr;
    }
    const float frac = subpixelX - std::floor(subpixelX);
    const auto step = static_cast<std::uint32_t>(std::min<float>(std::floor(frac * kSubpixelSteps + 0.5f), kSubpixelSteps - 1));
    const Key key{face.uniqueId(), glyph, static_cast<std::uint32_t>(std::lround(pixelSize * 64.0f)) << 2 | step};
    if (const auto it = m_glyphs.find(key); it != m_glyphs.end()) {
        ++m_hits;
        return &it->second;
    }
    ++m_misses;
    GlyphMask box;
    if (!render(face, glyph, static_cast<float>(key.size >> 2) / 64.0f, static_cast<float>(step) / kSubpixelSteps, box,
                m_scratch)) {
        return nullptr;
    }
    if (box.width > m_pageSize / 4 || box.height > m_pageSize / 4) {
        return nullptr;
    }
    if (box.width > 0) {
        std::optional<Recti> at;
        if (!m_pages.empty()) {
            at = m_pages.back()->packer.insert(box.width, box.height);
        }
        if (!at) {
            m_pages.push_back(std::make_unique<Page>(m_pageSize));
            at = m_pages.back()->packer.insert(box.width, box.height);
            if (!at) {
                return nullptr;
            }
        }
        Page &page = *m_pages.back();
        for (int y = 0; y < box.height; ++y) {
            std::memcpy(page.pixels.data() + static_cast<std::size_t>(at->y + y) * static_cast<std::size_t>(m_pageSize) +
                            static_cast<std::size_t>(at->x),
                        m_scratch.data() + static_cast<std::size_t>(y) * box.width, box.width);
        }
        ++page.generation;
        box.page = static_cast<std::uint16_t>(m_pages.size() - 1);
        box.x = static_cast<std::uint16_t>(at->x);
        box.y = static_cast<std::uint16_t>(at->y);
    }
    return &m_glyphs.emplace(key, box).first->second;
}

} // namespace cfw
