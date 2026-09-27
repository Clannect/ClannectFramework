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

} // namespace cfw
