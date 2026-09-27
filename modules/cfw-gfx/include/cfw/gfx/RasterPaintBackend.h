#pragma once

// The CPU backend: paints into a cfw::Image with anti-aliased coverage from
// cfw::Rasterizer, in 8-bit premultiplied sRGB, the arithmetic of Qt's raster
// engine (so renderings diff closely against Qt). It is the reference the GPU
// backend is checked against, and renders headless screenshots and
// diagnostics.
//
// - Gradients: a 1024-entry colour table interpolated in premultiplied
//   colour, sampled at pixel centres.
// - Images: bilinear (or nearest) sampling of premultiplied texels, clamped
//   to the source rectangle's pixels or repeated across it; straight-alpha
//   images are premultiplied as they are read.
// - Clips: a box of whole pixels plus, once a path or a rotated rectangle is
//   involved, an 8-bit coverage mask per level (masks are reused, so steady
//   state painting allocates nothing).
//
// Threads: one thread. Allocates: target-sized scratch rows and masks, kept
// between frames.

#include <array>
#include <cstdint>
#include <vector>

#include "cfw/gfx/PaintBackend.h"
#include "cfw/image/Rasterizer.h"

namespace cfw {

class RasterPaintBackend final : public PaintBackend {
public:
    // Paints into `target`, which must outlive the backend. A straight-alpha
    // target is premultiplied first (and stays premultiplied).
    explicit RasterPaintBackend(Image &target);

    [[nodiscard]] Vec2i size() const noexcept override;

    void fillPath(const PainterPath &path, const Transform2D &transform, FillRule rule, const Brush &brush,
                  const Transform2D &brushTransform, const CompositeState &state) override;
    void drawImage(const Image &image, const RectF &source, const RectF &target, const Transform2D &transform,
                   const ImageOptions &options, const CompositeState &state) override;
    void pushClipRect(const RectF &rect, const Transform2D &transform) override;
    void pushClipPath(const PainterPath &path, const Transform2D &transform, FillRule rule) override;
    void popClip() override;

private:
    struct ClipLevel {
        Recti box;
        int mask; // index into m_masks, or -1: the box alone
    };
    enum class SourceKind : std::uint8_t { Solid, Linear, Radial, Image };
    struct Source;

    [[nodiscard]] Recti clipBox() const noexcept;
    [[nodiscard]] const std::uint8_t *clipMask() const noexcept;
    void buildGradientTable(const Brush &brush);
    void paint(const Source &source, FillRule rule, const CompositeState &state);
    void fetch(const Source &source, int y, int x, int count, std::uint8_t *out) const;

    Image &m_target;
    int m_width = 0;
    int m_height = 0;
    Rasterizer m_rasterizer;
    PainterPath m_scratch; // rectangles for images and rotated clips
    std::vector<ClipLevel> m_clips;
    std::vector<std::vector<std::uint8_t>> m_masks;
    std::vector<std::uint8_t> m_coverage; // one row
    std::vector<std::uint8_t> m_pixels;   // one row of source pixels
    std::array<std::uint8_t, 1024 * 4> m_gradient{};
};

} // namespace cfw
