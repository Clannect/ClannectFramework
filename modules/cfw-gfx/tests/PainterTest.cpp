// Painter and the CPU backend, pixel by pixel: blend arithmetic, clip
// rounding and nesting, save/restore, opacity, gradient ends and seams,
// exact image blits, tiling, tint, cosmetic pens and hostile input.

#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

using Pixel = std::array<int, 4>;

Image canvas(std::uint32_t w, std::uint32_t h, Pixel fill = {0, 0, 0, 0}) {
    Image img = std::move(Image::create(w, h, AlphaMode::Premultiplied).value());
    Span<std::uint8_t> px = img.pixels();
    for (std::size_t i = 0; i < px.size(); ++i) {
        px[i] = static_cast<std::uint8_t>(fill[i % 4]);
    }
    return img;
}

Pixel at(const Image &img, int x, int y) {
    const std::uint8_t *p = img.row(static_cast<std::uint32_t>(y)).data() + x * 4;
    return {p[0], p[1], p[2], p[3]};
}

bool near(const Pixel &a, const Pixel &b, int tolerance = 0) {
    for (std::size_t k = 0; k < 4; ++k) {
        if (std::abs(a[k] - b[k]) > tolerance) {
            return false;
        }
    }
    return true;
}

Color rgba(int r, int g, int b, int a) {
    return Color::fromRgba8(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b),
                            static_cast<std::uint8_t>(a));
}

int covered(const Image &img) {
    int n = 0;
    for (int y = 0; y < static_cast<int>(img.height()); ++y) {
        for (int x = 0; x < static_cast<int>(img.width()); ++x) {
            n += at(img, x, y)[3] != 0 ? 1 : 0;
        }
    }
    return n;
}

void solidFillsAndBlending() {
    Image img = canvas(8, 8, {255, 255, 255, 255});
    {
        RasterPaintBackend backend(img);
        Painter p(backend);
        p.fillRect({2, 2, 4, 4}, rgba(255, 0, 0, 128));
    }
    // Premultiplied (128, 0, 0, 128) over opaque white: 128 + 255 * 127 / 255.
    check(near(at(img, 3, 3), {255, 127, 127, 255}), "half-transparent red over white");
    check(near(at(img, 1, 1), {255, 255, 255, 255}) && near(at(img, 6, 6), {255, 255, 255, 255}),
          "pixels outside the shape are untouched");

    Image red = canvas(4, 4, {255, 0, 0, 255});
    {
        RasterPaintBackend backend(red);
        Painter p(backend);
        p.setBlendMode(BlendMode::DestinationIn);
        p.fillRect({0, 0, 2, 4}, rgba(0, 0, 0, 128));
        p.setBlendMode(BlendMode::DestinationOut);
        p.fillRect({2, 0, 1, 4}, rgba(0, 0, 0, 255));
    }
    check(near(at(red, 0, 0), {128, 0, 0, 128}), "destination-in keeps the destination by source alpha");
    check(near(at(red, 2, 0), {0, 0, 0, 0}), "destination-out erases");
    check(near(at(red, 3, 0), {255, 0, 0, 255}), "outside the shape nothing changes, even for destination-in (Qt)");

    Image grey = canvas(2, 1, {128, 128, 128, 255});
    {
        RasterPaintBackend backend(grey);
        Painter p(backend);
        p.setBlendMode(BlendMode::Multiply);
        p.fillRect({0, 0, 1, 1}, rgba(255, 128, 0, 255));
        p.setBlendMode(BlendMode::Screen);
        p.fillRect({1, 0, 1, 1}, rgba(255, 128, 0, 255));
    }
    check(near(at(grey, 0, 0), {128, 64, 0, 255}, 1), "multiply");
    check(near(at(grey, 1, 0), {255, 192, 128, 255}, 1), "screen");

    Image half = canvas(4, 1);
    {
        RasterPaintBackend backend(half);
        Painter p(backend);
        p.fillRect({0.5f, 0, 2, 1}, rgba(0, 0, 255, 255));
    }
    check(near(at(half, 0, 0), {0, 0, 128, 128}) && near(at(half, 1, 0), {0, 0, 255, 255}) &&
              near(at(half, 2, 0), {0, 0, 128, 128}),
          "anti-aliased edges: half-covered pixels get half the colour");
}

void opacityAndState() {
    Image img = canvas(4, 4);
    {
        RasterPaintBackend backend(img);
        Painter p(backend);
        p.save();
        p.setOpacity(0.5f);
        p.translate(2, 0);
        p.fillRect({0, 0, 2, 2}, rgba(255, 255, 255, 255));
        p.restore();
        checkEqual(p.opacity(), 1.0f, "restore brings the opacity back");
        check(p.transform().isIdentity(), "and the transform");
        p.fillRect({0, 2, 1, 1}, rgba(255, 255, 255, 255));
        p.setOpacity(0.0f);
        p.fillRect({0, 0, 4, 4}, rgba(255, 0, 0, 255));
        p.setOpacity(std::numeric_limits<float>::quiet_NaN());
        checkEqual(p.opacity(), 1.0f, "a NaN opacity counts as 1");
        p.setOpacity(7.0f);
        checkEqual(p.opacity(), 1.0f, "opacity is clamped");
        p.restore(); // unmatched: ignored
    }
    check(near(at(img, 2, 0), {127, 127, 127, 127}), "opacity 0.5: coverage 255 * 128 >> 8 = 127, as in Qt");
    check(near(at(img, 0, 2), {255, 255, 255, 255}), "after restore, full opacity");
    check(near(at(img, 3, 3), {0, 0, 0, 0}), "opacity 0 draws nothing");
}

void clipping() {
    Image img = canvas(10, 10);
    RasterPaintBackend backend(img);
    {
        Painter p(backend);
        p.save();
        p.clipRect({1.5f, 1.5f, 3, 3}); // edges round to 2 and 5, as in Qt
        p.fillRect({0, 0, 10, 10}, rgba(255, 255, 255, 255));
        p.restore();
    }
    check(covered(img) == 9 && at(img, 2, 2)[3] == 255 && at(img, 4, 4)[3] == 255 && at(img, 5, 5)[3] == 0,
          "an axis-aligned clip rectangle covers whole pixels, edges rounded");

    Image nested = canvas(10, 10);
    RasterPaintBackend nestedBackend(nested);
    {
        Painter p(nestedBackend);
        p.clipRect({0, 0, 6, 10});
        p.clipRect({4, 0, 6, 10});
        PainterPath circle;
        circle.addEllipse({0, 0, 10, 10});
        p.clipPath(circle);
        p.fillRect({0, 0, 10, 10}, rgba(255, 255, 255, 255));
    } // the destructor pops all three clips
    check(covered(nested) > 0 && at(nested, 3, 5)[3] == 0 && at(nested, 6, 5)[3] == 0 && at(nested, 5, 5)[3] == 255,
          "nested clips intersect");
    check(at(nested, 4, 0)[3] > 0 && at(nested, 4, 0)[3] < 255, "path clips have anti-aliased edges");
    {
        Painter p(nestedBackend);
        p.fillRect({0, 0, 10, 10}, rgba(0, 0, 255, 255));
    }
    check(near(at(nested, 0, 0), {0, 0, 255, 255}), "a painter's clips end with it");

    Image rotated = canvas(20, 20);
    RasterPaintBackend rotatedBackend(rotated);
    {
        Painter p(rotatedBackend);
        p.translate(10, 10);
        p.rotate(45);
        p.clipRect({-5, -5, 10, 10});
        p.rotate(-45);
        p.fillRect({-10, -10, 20, 20}, rgba(255, 255, 255, 255));
    }
    check(at(rotated, 10, 10)[3] == 255 && at(rotated, 10, 5)[3] == 255 && at(rotated, 10, 3)[3] < 255 &&
              at(rotated, 4, 4)[3] == 0,
          "a rotated clip rectangle is a diamond");
    Image empty = canvas(4, 4);
    RasterPaintBackend emptyBackend(empty);
    {
        Painter p(emptyBackend);
        p.clipRect({1, 1, 1, 1});
        p.clipRect({3, 3, 1, 1});
        p.fillRect({0, 0, 4, 4}, rgba(255, 255, 255, 255));
    }
    checkEqual(covered(empty), 0, "disjoint clips leave nothing");
}

void gradients() {
    Image img = canvas(100, 3);
    {
        RasterPaintBackend backend(img);
        Painter p(backend);
        const std::array<GradientStop, 2> stops{GradientStop{0, rgba(255, 0, 0, 255)}, GradientStop{1, rgba(0, 0, 255, 255)}};
        p.fillRect({0, 0, 100, 1}, Brush::linearGradient({10, 0}, {90, 0}, stops));
        p.fillRect({0, 1, 100, 1}, Brush::linearGradient({0, 0}, {50, 0}, stops, GradientSpread::Repeat));
        const std::array<GradientStop, 2> fade{GradientStop{0, rgba(255, 0, 0, 255)}, GradientStop{1, rgba(0, 0, 255, 0)}};
        p.fillRect({0, 2, 100, 1}, Brush::linearGradient({0.5f, 0}, {98.5f, 0}, fade));
    }
    check(near(at(img, 2, 0), {255, 0, 0, 255}) && near(at(img, 97, 0), {0, 0, 255, 255}),
          "pad: the end colours continue");
    check(near(at(img, 50, 0), {126, 0, 129, 255}, 1), "sampled at the pixel centre: t = (50.5 - 10) / 80");
    check(near(at(img, 49, 1), {0, 0, 255, 255}, 6) && near(at(img, 50, 1), {255, 0, 0, 255}, 6),
          "repeat: the gradient starts over");
    check(near(at(img, 49, 2), {128, 0, 0, 128}, 1), "fading to transparent does not darken (premultiplied, as Qt)");

    Image radial = canvas(21, 21);
    {
        RasterPaintBackend backend(radial);
        Painter p(backend);
        const std::array<GradientStop, 2> stops{GradientStop{0, rgba(255, 255, 255, 255)},
                                                GradientStop{1, rgba(0, 0, 0, 255)}};
        p.fillRect({0, 0, 21, 21}, Brush::radialGradient({10.5f, 10.5f}, 10, stops));
    }
    check(near(at(radial, 10, 10), {255, 255, 255, 255}, 1), "radial: the centre is the first stop");
    check(near(at(radial, 0, 10), {0, 0, 0, 255}, 13), "and the circle the last");
    check(near(at(radial, 10, 5), at(radial, 5, 10)) && near(at(radial, 15, 10), at(radial, 10, 15)),
          "it is symmetric");
}

Image testImage() {
    Image img = std::move(Image::create(4, 2).value()); // straight alpha
    const std::array<Pixel, 8> px{Pixel{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 128},
                                  {10, 20, 30, 255},    {40, 50, 60, 0},  {70, 80, 90, 255}, {200, 100, 50, 64}};
    for (std::size_t i = 0; i < px.size(); ++i) {
        for (std::size_t k = 0; k < 4; ++k) {
            img.pixels()[i * 4 + k] = static_cast<std::uint8_t>(px[i][k]);
        }
    }
    return img;
}

void images() {
    const Image src = testImage();
    for (const bool smooth : {false, true}) {
        Image img = canvas(6, 4);
        {
            RasterPaintBackend backend(img);
            Painter p(backend);
            ImageOptions o;
            o.smooth = smooth;
            p.drawImage({1, 1, 4, 2}, src, o);
        }
        bool exact = true;
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 4; ++x) {
                const std::uint8_t *s = src.row(static_cast<std::uint32_t>(y)).data() + x * 4;
                const int a = s[3];
                const Pixel pre{(s[0] * a + 127) / 255, (s[1] * a + 127) / 255, (s[2] * a + 127) / 255, a};
                exact = exact && near(at(img, x + 1, y + 1), pre, 1);
            }
        }
        check(exact, smooth ? "an unscaled smooth blit copies every pixel (premultiplied)"
                            : "an unscaled nearest blit copies every pixel (premultiplied)");
        check(near(at(img, 0, 0), {0, 0, 0, 0}) && near(at(img, 5, 3), {0, 0, 0, 0}), "and nothing else");
    }
    // Tiling: a 4 x 2 image repeated over 10 x 6, starting at the target's corner.
    Image tiled = canvas(12, 8);
    {
        RasterPaintBackend backend(tiled);
        Painter p(backend);
        ImageOptions o;
        o.smooth = false;
        o.tileSize = {4, 2};
        p.drawImage({1, 1, 10, 6}, src, o);
    }
    check(near(at(tiled, 1, 1), at(tiled, 5, 3)) && near(at(tiled, 2, 2), at(tiled, 10, 6)) &&
              near(at(tiled, 1, 1), {255, 0, 0, 255}),
          "tiling repeats the image from the target's corner");
    check(near(at(tiled, 11, 7), {0, 0, 0, 0}), "within the target only");
    // Tint multiplies; white leaves the image alone.
    Image tinted = canvas(4, 2);
    {
        RasterPaintBackend backend(tinted);
        Painter p(backend);
        ImageOptions o;
        o.smooth = false;
        o.tint = rgba(255, 128, 0, 255);
        p.drawImage({0, 0, 4, 2}, src, o);
    }
    check(near(at(tinted, 1, 0), {0, 128, 0, 255}, 1) && near(at(tinted, 0, 0), {255, 0, 0, 255}, 1),
          "tint multiplies each channel");
    // A 2x upscale samples between pixels.
    Image scaled = canvas(8, 4);
    {
        RasterPaintBackend backend(scaled);
        Painter p(backend);
        p.drawImage({0, 0, 8, 4}, src);
    }
    const Pixel mid = at(scaled, 1, 0);
    check(mid[0] > 100 && mid[1] > 30 && mid[0] < 255 && mid[1] < 200, "smooth upscaling blends neighbours");
    // Straight-alpha targets are premultiplied first.
    Image straight = std::move(Image::create(1, 1).value());
    straight.pixels()[0] = 255;
    straight.pixels()[3] = 128;
    {
        RasterPaintBackend backend(straight);
    }
    check(straight.alphaMode() == AlphaMode::Premultiplied && straight.pixels()[0] == 128,
          "a straight-alpha target is premultiplied");
}

void pensAndHostileInput() {
    Image img = canvas(40, 20);
    {
        RasterPaintBackend backend(img);
        Painter p(backend);
        p.scale(4, 4);
        Pen cosmetic;
        cosmetic.width = 0;
        cosmetic.brush = rgba(255, 255, 255, 255);
        p.drawLine({1, 2.625f}, {9, 2.625f}, cosmetic); // device y = 10.5: exactly one row
    }
    int rows = 0;
    for (int y = 0; y < 20; ++y) {
        rows += at(img, 20, y)[3] != 0 ? 1 : 0;
    }
    checkEqual(rows, 1, "a cosmetic pen stays one pixel wide under a 4x scale");

    Image hostile = canvas(16, 16);
    {
        RasterPaintBackend backend(hostile);
        Painter p(backend);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        p.fillRect({nan, 0, 5, 5}, rgba(255, 0, 0, 255));
        p.fillRect({-1e30f, -1e30f, 2e30f, 2e30f}, rgba(0, 255, 0, 255));
        p.setTransform(Transform2D::scaling(0, 0));
        p.fillRect({0, 0, 5, 5}, rgba(0, 0, 255, 255));
        p.clipRect({0, 0, 5, 5});
        const Image nothing;
        p.drawImage({0, 0, 4, 4}, nothing);
        p.drawImage({0, 0, 4, 4}, testImage(), RectF{10, 10, 5, 5}); // source outside the image
    }
    check(near(at(hostile, 8, 8), {0, 255, 0, 255}), "a huge rectangle covers the target; NaN and degenerate draw nothing");
}

} // namespace

int main() {
    solidFillsAndBlending();
    opacityAndState();
    clipping();
    gradients();
    images();
    pensAndHostileInput();
    return cfw::test::finish("PainterTest");
}
