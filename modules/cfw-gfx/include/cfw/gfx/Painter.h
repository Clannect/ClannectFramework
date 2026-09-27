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
};

} // namespace cfw
