#pragma once

// The seam between Painter and whatever draws: the CPU backend
// (RasterPaintBackend) today, a GPU backend later. Backends receive
// resolved commands (the Painter already applied save/restore state and
// turned strokes into fills), so every backend sees the same few primitives.
//
// Threads: a backend is used from one thread at a time.

#include <cstddef>
#include <cstdint>

#include "cfw/core/PainterPath.h"
#include "cfw/core/Rect.h"
#include "cfw/core/Span.h"
#include "cfw/core/Transform2D.h"
#include "cfw/gfx/BlendMode.h"
#include "cfw/gfx/Brush.h"
#include "cfw/gfx/ImageOptions.h"

namespace cfw {

class Image;

// Painter state that applies to every drawing command.
struct CompositeState {
    float opacity = 1.0f; // 0 to 1, multiplies the source
    BlendMode blend = BlendMode::SourceOver;
};

class PaintBackend {
public:
    virtual ~PaintBackend() = default;

    // Pixel size of the target.
    [[nodiscard]] virtual Vec2i size() const noexcept = 0;

    // Fills `path`, mapped by `transform`, with `brush`, whose geometry
    // (gradient points) is mapped by `brushTransform`.
    virtual void fillPath(const PainterPath &path, const Transform2D &transform, FillRule rule, const Brush &brush,
                          const Transform2D &brushTransform, const CompositeState &state) = 0;

    // Draws `source` (in image pixels) of `image` over `target`, mapped by
    // `transform`.
    virtual void drawImage(const Image &image, const RectF &source, const RectF &target,
                           const Transform2D &transform, const ImageOptions &options,
                           const CompositeState &state) = 0;

    // Fills 8-bit coverage masks (glyph images) placed at whole pixels with
    // `brush`: each mask's rows cover its target rectangle.
    struct MaskBlit {
        const std::uint8_t *pixels;
        std::ptrdiff_t stride;
        Recti target;
    };
    virtual void fillMasks(Span<const MaskBlit> masks, const Brush &brush, const Transform2D &brushTransform,
                           const CompositeState &state) = 0;

    // Clips later drawing to the intersection with the given area until the
    // matching popClip(). Rectangles have their own call so backends can use
    // scissoring; axis-aligned ones clip to whole pixels, rounding each edge
    // to the nearest pixel boundary, as Qt does. Paths clip with
    // anti-aliased edges.
    virtual void pushClipRect(const RectF &rect, const Transform2D &transform) = 0;
    virtual void pushClipPath(const PainterPath &path, const Transform2D &transform, FillRule rule) = 0;
    virtual void popClip() = 0;
};

} // namespace cfw
