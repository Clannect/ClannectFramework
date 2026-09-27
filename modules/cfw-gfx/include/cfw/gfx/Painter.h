#pragma once

// Draws onto a PaintBackend: QPainter's model with anti-aliasing always on.
//
// State (transform, opacity, blend mode, clip) is saved and restored as a
// stack; clips nest by intersection and are undone by restore() (or by the
// Painter's destructor). Strokes become fills here, through the Stroker, so
// every backend draws them identically; cosmetic pens (width 0, or
// Pen::cosmetic) are stroked after the transform, in device pixels.
//
// Fills default to the non-zero rule. (QPainterPath defaults to even-odd;
// for the shapes the engine draws the two agree.)
//
// Threads: one thread. Allocates: scratch storage for strokes, reused; the
// state stack beyond eight levels.

#include <cstddef>

#include "cfw/core/SmallVector.h"
#include "cfw/gfx/PaintBackend.h"
#include "cfw/text/GlyphCache.h"
#include "cfw/text/TextLayout.h"
#include "cfw/gfx/Pen.h"
#include "cfw/gfx/Stroker.h"

namespace cfw {

class Painter {
public:
    explicit Painter(PaintBackend &backend) noexcept : m_backend(backend) {}
    // Undoes every clip this painter pushed.
    ~Painter();
    Painter(const Painter &) = delete;
    Painter &operator=(const Painter &) = delete;

    [[nodiscard]] PaintBackend &backend() noexcept { return m_backend; }

    void save();
    // Does nothing without a matching save().
    void restore();

    // Replaces the transform, or with `combine` applies `t` first and then
    // the current transform (QPainter::setTransform(t, true)).
    void setTransform(const Transform2D &t, bool combine = false) noexcept;
    [[nodiscard]] const Transform2D &transform() const noexcept { return m_state.transform; }
    void translate(float dx, float dy) noexcept;
    void scale(float sx, float sy) noexcept;
    void rotate(float degrees) noexcept;

    // Absolute, like QPainter::setOpacity; clamped to [0, 1].
    void setOpacity(float opacity) noexcept;
    [[nodiscard]] float opacity() const noexcept { return m_state.composite.opacity; }
    void setBlendMode(BlendMode mode) noexcept { m_state.composite.blend = mode; }
    [[nodiscard]] BlendMode blendMode() const noexcept { return m_state.composite.blend; }

    // Intersects the clip with an area in the current coordinates.
    void clipRect(const RectF &rect);
    void clipPath(const PainterPath &path, FillRule rule = FillRule::NonZero);

    void fillPath(const PainterPath &path, const Brush &brush, FillRule rule = FillRule::NonZero);
    void fillRect(const RectF &rect, const Brush &brush);
    void strokePath(const PainterPath &path, const Pen &pen);
    void strokeRect(const RectF &rect, const Pen &pen);
    void drawLine(Vec2 from, Vec2 to, const Pen &pen);

    // Draws `source` (image pixels) of `image` into `target`.
    void drawImage(const RectF &target, const Image &image, const RectF &source, const ImageOptions &options = {});
    // The whole image.
    void drawImage(const RectF &target, const Image &image, const ImageOptions &options = {});

    // Draws laid-out text with the layout's top-left at `origin`. Under a
    // plain translation glyphs come from the glyph cache (masks at quarter-
    // pixel positions); under any other transform, and for glyphs too large
    // for the atlas, they are filled as outlines.
    void drawText(const TextLayout &layout, Vec2 origin, const Brush &brush);
    // Positioned glyphs of one face (positions on the baseline, relative to
    // `origin`), as drawText() draws them.
    struct PositionedGlyph {
        GlyphId glyph;
        Vec2 position;
    };
    void drawGlyphs(const FontFace &face, float pixelSize, Span<const PositionedGlyph> glyphs, Vec2 origin,
                    const Brush &brush);
    // The glyph cache to draw from (default: one per thread).
    void setGlyphCache(GlyphCache *cache) noexcept { m_glyphCache = cache; }

private:
    struct State {
        Transform2D transform;
        CompositeState composite;
        std::size_t clipDepth = 0;
    };

    PaintBackend &m_backend;
    State m_state;
    SmallVector<State, 8> m_saved;
    std::size_t m_clipDepth = 0;
    Stroker m_stroker;
    PainterPath m_scratch;
    PainterPath m_stroke;
    PainterPath m_device; // cosmetic strokes: the path in device space
    PainterPath m_glyph;
    GlyphCache *m_glyphCache = nullptr;
    std::vector<PaintBackend::MaskBlit> m_masks;
    void drawGlyph(const FontFace &face, float pixelSize, GlyphId glyph, Vec2 position, bool masks);
    void flushGlyphs(const Brush &brush);
};

} // namespace cfw
