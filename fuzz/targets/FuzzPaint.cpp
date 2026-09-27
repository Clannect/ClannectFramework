// Painter, stroker and rasteriser on hostile geometry: the bytes are a
// program of paths (with NaN, infinite and huge coordinates), pens, brushes,
// transforms (degenerate and perspective ones included), clips, blend modes
// and images, run on a small canvas. Nothing may crash or hang, and every
// pixel must stay valid premultiplied colour (no channel above alpha).

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>

#include "FuzzTarget.h"
#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"

namespace {

struct Reader {
    const std::uint8_t *data;
    std::size_t size;
    std::size_t at = 0;

    bool done() const { return at >= size; }
    std::uint8_t byte() { return at < size ? data[at++] : 0; }
    // Mostly ordinary coordinates, sometimes hostile ones.
    float number() {
        const std::uint8_t kind = byte();
        const auto raw = static_cast<std::int16_t>(static_cast<std::uint16_t>(byte() | (byte() << 8)));
        switch (kind % 16) {
        case 0: return std::numeric_limits<float>::quiet_NaN();
        case 1: return std::numeric_limits<float>::infinity();
        case 2: return -std::numeric_limits<float>::infinity();
        case 3: return static_cast<float>(raw) * 1e25f;
        case 4: return static_cast<float>(raw) * 1e-6f;
        default: return static_cast<float>(raw) / 256.0f;
        }
    }
    cfw::Vec2 point() { return {number(), number()}; }
    cfw::Color color() {
        return cfw::Color::fromRgba8(byte(), byte(), byte(), byte());
    }
};

cfw::PainterPath readPath(Reader &r) {
    cfw::PainterPath p;
    const int count = r.byte() % 12;
    for (int i = 0; i < count && !r.done(); ++i) {
        switch (r.byte() % 9) {
        case 0: p.moveTo(r.point()); break;
        case 1: p.lineTo(r.point()); break;
        case 2: p.quadTo(r.point(), r.point()); break;
        case 3: p.cubicTo(r.point(), r.point(), r.point()); break;
        case 4: p.close(); break;
        case 5: p.addRect({r.number(), r.number(), r.number(), r.number()}); break;
        case 6: p.addRoundedRect({r.number(), r.number(), r.number(), r.number()}, r.number(), r.number()); break;
        case 7: p.addEllipse({r.number(), r.number(), r.number(), r.number()}); break;
        default: p.arcTo({r.number(), r.number(), r.number(), r.number()}, r.number(), r.number()); break;
        }
    }
    return p;
}

cfw::Brush readBrush(Reader &r) {
    std::array<cfw::GradientStop, 3> stops{};
    const std::size_t n = r.byte() % 4;
    for (std::size_t i = 0; i < n; ++i) {
        stops[i] = {r.number(), r.color()};
    }
    const auto spread = static_cast<cfw::GradientSpread>(r.byte() % 3);
    switch (r.byte() % 4) {
    case 0: return {};
    case 1: return cfw::Brush::linearGradient(r.point(), r.point(), cfw::Span<const cfw::GradientStop>(stops.data(), n), spread);
    case 2: return cfw::Brush::radialGradient(r.point(), r.number(), r.point(), cfw::Span<const cfw::GradientStop>(stops.data(), n), spread);
    default: return r.color();
    }
}

cfw::Pen readPen(Reader &r) {
    cfw::Pen pen;
    pen.brush = readBrush(r);
    pen.width = r.number();
    pen.cap = static_cast<cfw::CapStyle>(r.byte() % 3);
    pen.join = static_cast<cfw::JoinStyle>(r.byte() % 4);
    pen.miterLimit = r.number();
    const int dashes = r.byte() % 5;
    for (int i = 0; i < dashes; ++i) {
        pen.dashes.push_back(r.number());
    }
    pen.dashOffset = r.number();
    pen.cosmetic = r.byte() % 4 == 0;
    return pen;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    Reader r{data, size};
    cfw::Image canvas = std::move(cfw::Image::create(48, 32, cfw::AlphaMode::Premultiplied).value());
    cfw::Image picture = std::move(cfw::Image::create(3, 2).value()); // straight alpha
    for (std::size_t i = 0; i < picture.pixels().size(); ++i) {
        picture.pixels()[i] = r.byte();
    }
    {
        cfw::RasterPaintBackend backend(canvas);
        cfw::Painter p(backend);
        for (int step = 0; step < 24 && !r.done(); ++step) {
            switch (r.byte() % 14) {
            case 0: p.save(); break;
            case 1: p.restore(); break;
            case 2: p.translate(r.number(), r.number()); break;
            case 3: p.scale(r.number(), r.number()); break;
            case 4: p.rotate(r.number()); break;
            case 5: {
                const std::array<cfw::Vec2, 4> from{r.point(), r.point(), r.point(), r.point()};
                const std::array<cfw::Vec2, 4> to{r.point(), r.point(), r.point(), r.point()};
                if (const std::optional<cfw::Transform2D> q = cfw::Transform2D::quadToQuad(from, to)) {
                    p.setTransform(*q, true);
                }
                break;
            }
            case 6: p.setOpacity(r.number()); break;
            case 7: p.setBlendMode(static_cast<cfw::BlendMode>(r.byte() % 7)); break;
            case 8: p.clipRect({r.number(), r.number(), r.number(), r.number()}); break;
            case 9: p.clipPath(readPath(r), r.byte() % 2 ? cfw::FillRule::EvenOdd : cfw::FillRule::NonZero); break;
            case 10: {
                const cfw::PainterPath path = readPath(r);
                p.fillPath(path, readBrush(r), r.byte() % 2 ? cfw::FillRule::EvenOdd : cfw::FillRule::NonZero);
                break;
            }
            case 11: {
                const cfw::PainterPath path = readPath(r);
                p.strokePath(path, readPen(r));
                break;
            }
            case 12: {
                cfw::ImageOptions o;
                o.smooth = r.byte() % 2 == 0;
                if (r.byte() % 2 == 0) {
                    o.tileSize = r.point();
                }
                o.tint = r.color();
                p.drawImage({r.number(), r.number(), r.number(), r.number()}, picture,
                            {r.number(), r.number(), r.number(), r.number()}, o);
                break;
            }
            default: p.fillRect({r.number(), r.number(), r.number(), r.number()}, readBrush(r)); break;
            }
        }
    }
    const cfw::Span<const std::uint8_t> px = canvas.pixels();
    for (std::size_t i = 0; i < px.size(); i += 4) {
        if (px[i] > px[i + 3] || px[i + 1] > px[i + 3] || px[i + 2] > px[i + 3]) {
            std::abort(); // not valid premultiplied colour
        }
    }
    return 0;
}
