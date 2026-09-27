#pragma once

// Icons as SVG: the subset interface icons use, drawn through Painter.
//
// Elements: svg (viewBox, width/height), g, path (all path commands,
// elliptical arcs included), rect (with rx/ry), circle, ellipse, line,
// polyline, polygon. Presentation attributes and simple `style="a:b"`:
// fill, stroke (none, #rgb, #rrggbb, rgb(), currentColor and the basic
// named colours), stroke-width, stroke-linecap, stroke-linejoin,
// stroke-miterlimit, fill-rule, opacity, fill-opacity, stroke-opacity, and
// transform (matrix, translate, scale, rotate, skewX, skewY), inherited
// through groups. Other elements (defs, text, gradients, masks, ...) are
// skipped. Rendering maps the viewBox onto the target rectangle as
// QSvgRenderer::render does, and is compared with it in SvgImageTest.
//
// Threads: an SvgImage is immutable once parsed; render from any thread.

#include <optional>
#include <vector>

#include "cfw/core/Color.h"
#include "cfw/core/PainterPath.h"
#include "cfw/core/Rect.h"
#include "cfw/core/Result.h"
#include "cfw/core/String.h"
#include "cfw/core/Transform2D.h"
#include "cfw/gfx/Pen.h"

namespace cfw {

class Painter;

// Parses SVG path data ("M10 10 h5 a2 2 0 0 1 2 2 z") into `out`
// (appending). False on malformed data; what came before the error is kept,
// as SVG renderers do.
bool parseSvgPathData(StringView data, PainterPath &out);

class SvgImage {
public:
    // Fails on anything that is not an <svg> document.
    [[nodiscard]] static Result<SvgImage> parse(StringView svg);

    [[nodiscard]] RectF viewBox() const noexcept { return m_viewBox; }

    // Draws the image into `target`; `currentColor` is what fill or stroke
    // "currentColor" means.
    void render(Painter &painter, const RectF &target, Color currentColor = Color{0, 0, 0, 1}) const;

    struct Paint {
        enum class Kind : std::uint8_t { None, Color, CurrentColor } kind = Kind::None;
        Color color{};
    };
    struct Shape {
        PainterPath path;
        Transform2D transform;
        Paint fill;
        Paint stroke;
        float opacity = 1; // the element's (painter opacity, as in Qt)
        float fillOpacity = 1;
        float strokeOpacity = 1;
        FillRule fillRule = FillRule::NonZero;
        float strokeWidth = 1;
        CapStyle cap = CapStyle::Flat;
        JoinStyle join = JoinStyle::SvgMiter;
        float miterLimit = 4;
    };
    [[nodiscard]] const std::vector<Shape> &shapes() const noexcept { return m_shapes; }

private:
    RectF m_viewBox{};
    std::vector<Shape> m_shapes;
};

} // namespace cfw
