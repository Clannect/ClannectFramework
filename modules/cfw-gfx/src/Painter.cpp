#include "cfw/gfx/Painter.h"

#include <algorithm>
#include <cmath>

#include "cfw/image/Image.h"

namespace cfw {

namespace {

// Curves are flattened to this many device pixels.
constexpr float kDeviceTolerance = 0.1f;

float toleranceFor(const Transform2D &t, const RectF &bounds) {
    return static_cast<float>(kDeviceTolerance / t.maxStretch(bounds));
}

} // namespace

Painter::~Painter() {
    while (m_clipDepth > 0) {
        m_backend.popClip();
        --m_clipDepth;
    }
}

void Painter::save() {
    m_state.clipDepth = m_clipDepth;
    m_saved.push_back(m_state);
}

void Painter::restore() {
    if (m_saved.empty()) {
        return;
    }
    m_state = m_saved.back();
    m_saved.pop_back();
    while (m_clipDepth > m_state.clipDepth) {
        m_backend.popClip();
        --m_clipDepth;
    }
}

void Painter::setTransform(const Transform2D &t, bool combine) noexcept {
    m_state.transform = combine ? m_state.transform * t : t;
}

void Painter::translate(float dx, float dy) noexcept { setTransform(Transform2D::translation(dx, dy), true); }
void Painter::scale(float sx, float sy) noexcept { setTransform(Transform2D::scaling(sx, sy), true); }
void Painter::rotate(float degrees) noexcept { setTransform(Transform2D::rotation(degrees), true); }

void Painter::setOpacity(float opacity) noexcept {
    m_state.composite.opacity = std::isfinite(opacity) ? std::clamp(opacity, 0.0f, 1.0f) : 1.0f;
}

void Painter::clipRect(const RectF &rect) {
    m_backend.pushClipRect(rect, m_state.transform);
    ++m_clipDepth;
}

void Painter::clipPath(const PainterPath &path, FillRule rule) {
    m_backend.pushClipPath(path, m_state.transform, rule);
    ++m_clipDepth;
}

void Painter::fillPath(const PainterPath &path, const Brush &brush, FillRule rule) {
    if (brush.isNone() || path.empty()) {
        return;
    }
    m_backend.fillPath(path, m_state.transform, rule, brush, m_state.transform, m_state.composite);
}

void Painter::fillRect(const RectF &rect, const Brush &brush) {
    m_scratch.clear();
    m_scratch.addRect(rect);
    fillPath(m_scratch, brush);
}

void Painter::strokePath(const PainterPath &path, const Pen &pen) {
    if (pen.brush.isNone() || path.empty()) {
        return;
    }
    const Transform2D &t = m_state.transform;
    if (!pen.isCosmetic()) {
        m_stroker.stroke(path, pen, toleranceFor(t, path.controlBounds()), m_stroke);
        m_backend.fillPath(m_stroke, t, FillRule::NonZero, pen.brush, t, m_state.composite);
        return;
    }
    // Cosmetic: stroke in device space, so the width is in pixels.
    const PainterPath *device = &path;
    PainterPath &mapped = m_device;
    mapped.clear();
    if (!t.isIdentity()) {
        if (t.isAffine()) {
            mapped.addPath(path);
            mapped.transform(t);
        } else {
            // Curves do not survive a perspective map: flatten, then map.
            for (const PainterPath::Polyline &poly : path.flatten(toleranceFor(t, path.controlBounds()))) {
                for (std::size_t i = 0; i < poly.points.size(); ++i) {
                    const Vec2 p = t.map(poly.points[i]);
                    if (i == 0) {
                        mapped.moveTo(p);
                    } else {
                        mapped.lineTo(p);
                    }
                }
                if (poly.closed) {
                    mapped.close();
                }
            }
        }
        device = &mapped;
    }
    Pen devicePen = pen;
    devicePen.width = pen.width == 0.0f ? 1.0f : pen.width;
    m_stroker.stroke(*device, devicePen, kDeviceTolerance, m_stroke);
    m_backend.fillPath(m_stroke, Transform2D{}, FillRule::NonZero, pen.brush, t, m_state.composite);
}

void Painter::strokeRect(const RectF &rect, const Pen &pen) {
    m_scratch.clear();
    m_scratch.addRect(rect);
    strokePath(m_scratch, pen);
}

void Painter::drawLine(Vec2 from, Vec2 to, const Pen &pen) {
    m_scratch.clear();
    m_scratch.moveTo(from);
    m_scratch.lineTo(to);
    strokePath(m_scratch, pen);
}

void Painter::drawImage(const RectF &target, const Image &image, const RectF &source, const ImageOptions &options) {
    if (image.empty()) {
        return;
    }
    m_backend.drawImage(image, source, target, m_state.transform, options, m_state.composite);
}

void Painter::drawImage(const RectF &target, const Image &image, const ImageOptions &options) {
    drawImage(target, image,
              RectF{0.0f, 0.0f, static_cast<float>(image.width()), static_cast<float>(image.height())}, options);
}

// ---- Text ----

namespace {

thread_local GlyphCache t_glyphCache;

bool isTranslation(const Transform2D &t) {
    return t.isAffine() && t(0, 0) == 1.0 && t(1, 1) == 1.0 && t(0, 1) == 0.0 && t(1, 0) == 0.0;
}

} // namespace

void Painter::drawGlyph(const FontFace &face, float pixelSize, GlyphId glyph, Vec2 position, bool masks) {
    if (masks) {
        GlyphCache &cache = m_glyphCache ? *m_glyphCache : t_glyphCache;
        const Vec2 p = m_state.transform.map(position);
        if (std::isfinite(p.x) && std::isfinite(p.y) && std::abs(p.x) < 1e7f && std::abs(p.y) < 1e7f) {
            const float x = std::floor(p.x);
            if (const GlyphMask *m = cache.glyph(face, glyph, pixelSize, p.x - x)) {
                if (m->width > 0) {
                    const int px = static_cast<int>(x) + m->left;
                    const int py = static_cast<int>(std::lround(p.y)) + m->top;
                    const Span<const std::uint8_t> page = cache.page(m->page);
                    m_masks.push_back({page.data() + static_cast<std::size_t>(m->y) * static_cast<std::size_t>(cache.pageSize()) + m->x,
                                       cache.pageSize(), Recti{px, py, m->width, m->height}});
                }
                return;
            }
        }
    }
    // As an outline: font units, y up, scaled and placed.
    if (!face.glyphOutline(glyph, m_glyph) || m_glyph.empty()) {
        return;
    }
    const double scale = static_cast<double>(pixelSize) / face.unitsPerEm();
    m_glyph.transform(Transform2D::scaling(scale, -scale).then(Transform2D::translation(position.x, position.y)));
    m_scratch.addPath(m_glyph);
}

void Painter::flushGlyphs(const Brush &brush) {
    if (!m_masks.empty()) {
        m_backend.fillMasks(m_masks, brush, m_state.transform, m_state.composite);
        m_masks.clear();
    }
    if (!m_scratch.empty()) {
        m_backend.fillPath(m_scratch, m_state.transform, FillRule::NonZero, brush, m_state.transform, m_state.composite);
        m_scratch.clear();
    }
}

void Painter::drawGlyphs(const FontFace &face, float pixelSize, Span<const PositionedGlyph> glyphs, Vec2 origin,
                         const Brush &brush) {
    if (brush.isNone() || !(pixelSize > 0.0f) || !std::isfinite(pixelSize)) {
        return;
    }
    const bool masks = isTranslation(m_state.transform);
    m_scratch.clear();
    for (const PositionedGlyph &g : glyphs) {
        drawGlyph(face, pixelSize, g.glyph, {origin.x + g.position.x, origin.y + g.position.y}, masks);
    }
    flushGlyphs(brush);
}

void Painter::drawText(const TextLayout &layout, Vec2 origin, const Brush &brush) {
    const float size = layout.pixelSize();
    if (brush.isNone() || !(size > 0.0f)) {
        return;
    }
    const bool masks = isTranslation(m_state.transform);
    m_scratch.clear();
    const Span<const std::shared_ptr<const FontFace>> fonts = layout.fonts();
    for (const TextLayout::Glyph &g : layout.glyphs()) {
        if (g.font < fonts.size()) {
            drawGlyph(*fonts[g.font], size, g.glyph, {origin.x + g.x, origin.y + g.y}, masks);
        }
    }
    flushGlyphs(brush);
}

} // namespace cfw
