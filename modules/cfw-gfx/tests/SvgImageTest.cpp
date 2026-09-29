// SVG icons against Qt: every file in testdata/svg rendered at 16, 20, 24
// and 32 pixels as Clannect's editor renders its icons, compared with
// QSvgRenderer's output (testdata/golden-svg, testing/qt-oracle/
// render_svg.py). Strokes are kept wider than one pixel: Qt draws thinner
// pens with its cosmetic line stroker (lighter than exact coverage), which
// CFW does not imitate (decision 0014). Then the path-data parser and the
// document reader on edge cases and malformed input.

#include "cfw/gfx/SvgImage.h"

#include <cmath>
#include <cstdio>

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/image/Png.h"
#include "cfw/io/FileSystem.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

const Path kData = Path(CFW_GFX_TESTDATA);

// True if the pixel and its neighbours have one colour (so coverage
// differences along edges do not count as colour differences).
bool flat(const Image &img, std::size_t index, std::size_t width) {
    const std::size_t x = index % width;
    const std::size_t y = index / width;
    if (x == 0 || y == 0 || x + 1 >= width || y + 1 >= img.height()) {
        return false;
    }
    const std::uint8_t *c = img.pixels().data() + index * 4;
    if (c[3] < 250) {
        return false;
    }
    for (std::size_t dy = y - 1; dy <= y + 1; ++dy) {
        for (std::size_t dx = x - 1; dx <= x + 1; ++dx) {
            const std::uint8_t *n = img.pixels().data() + (dy * width + dx) * 4;
            for (int k = 0; k < 4; ++k) {
                if (std::abs(int(n[k]) - int(c[k])) > 2) {
                    return false;
                }
            }
        }
    }
    return true;
}

void againstQt() {
    const Result<std::vector<DirectoryEntry>> files = listDirectory(kData / "svg");
    check(files.ok() && files.value().size() >= 10, "the test icons are there");
    if (!files) {
        return;
    }
    for (const DirectoryEntry &f : files.value()) {
        const Result<String> text = readTextFile(f.path);
        const Result<SvgImage> svg = text ? SvgImage::parse(text.value()) : Result<SvgImage>(text.error());
        check(svg.ok(), "the icon parses");
        if (!svg) {
            continue;
        }
        for (const int size : {16, 20, 24, 32}) {
            const int canvas = size + 4;
            Image mine = std::move(Image::create(static_cast<std::uint32_t>(canvas), static_cast<std::uint32_t>(canvas),
                                                 AlphaMode::Premultiplied)
                                       .value());
            {
                RasterPaintBackend backend(mine);
                Painter painter(backend);
                svg.value().render(painter, {2, 2, static_cast<float>(size), static_cast<float>(size)});
            }
            const String golden = f.path.stem() + "-" + std::to_string(size) + ".png";
            const Result<std::vector<std::byte>> png = readFile(kData / "golden-svg" / golden);
            const Result<Image> qt = png ? decodePng(png.value()) : Result<Image>(png.error());
            check(qt.ok(), "the Qt image reads");
            if (!qt) {
                continue;
            }
            int worstAlpha = 0;
            int worstColour = 0;
            double diff = 0;
            double ink = 0;
            for (std::size_t i = 0; i < mine.pixels().size(); i += 4) {
                const std::uint8_t *a = mine.pixels().data() + i;
                const std::uint8_t *b = qt.value().pixels().data() + i;
                const int alpha = std::abs(int(a[3]) - int(b[3]));
                worstAlpha = std::max(worstAlpha, alpha);
                diff += alpha;
                ink += b[3];
                if (flat(qt.value(), i / 4, static_cast<std::size_t>(canvas))) { // colour away from edges
                    for (int k = 0; k < 3; ++k) {
                        const int straight = int(std::lround(a[k] * 255.0 / a[3]));
                        worstColour = std::max(worstColour, std::abs(straight - int(b[k])));
                    }
                }
            }
            const double relative = diff / std::max(ink, 1.0);
            // Qt flattens circles and arcs more coarsely: small icons differ by
            // a few percent of their ink along curves.
            const double tolerance = size <= 16 ? 0.06 : 0.05;
            if (relative > tolerance || worstColour > 3) {
                std::printf("  %s: alpha worst %d, %.2f%% of the ink; colour worst %d\n", golden.c_str(), worstAlpha,
                            relative * 100, worstColour);
            }
            check(relative <= tolerance, "coverage within 5% of Qt's ink (6% at 16 px)");
            check(worstColour <= 3, "colours within 3 levels of Qt's");
        }
    }
}

void pathData() {
    PainterPath p;
    check(parseSvgPathData("M4.037 4.688a.495.495 0 0 1 .651-.651l16 6.5z", p), "packed numbers and flags");
    check(p.currentPoint() == Vec2{4.037f, 4.688f}, "close returns to the start");
    p.clear();
    check(parseSvgPathData("m1 1 2 0 0 2h-2v-2z M10 10 20 10", p), "implicit line-tos after moves");
    check(std::abs(p.currentPoint().x - 20) < 1e-5f, "absolute pairs after M");
    p.clear();
    check(parseSvgPathData("M0 0C1 1 2 1 3 0S5 -1 6 0Q7 1 8 0T10 0", p), "curve shorthands");
    check(std::abs(p.currentPoint().x - 10) < 1e-5f, "end point");
    p.clear();
    check(parseSvgPathData("M1e1 2E+1l-1.5e-1.5", p), "exponents and a number starting with a dot");
    p.clear();
    check(!parseSvgPathData("M0 0 L 5", p), "a missing coordinate fails");
    check(p.verbs().size() == 1, "and keeps what came before");
    p.clear();
    check(!parseSvgPathData("10 10", p), "data must start with a command");
    check(!parseSvgPathData("M0 0 X 5 5", p), "unknown commands fail");
    // A semicircle arc of radius 5: its extent.
    p.clear();
    check(parseSvgPathData("M0 0 A5 5 0 0 1 10 0", p), "an arc");
    const RectF b = p.controlBounds();
    check(b.y < -4.9f && b.y > -7.0f, "the arc goes up (sweep 1, y down: clockwise on screen)");
    p.clear();
    check(parseSvgPathData("M0 0 A1 1 0 0 1 10 0", p), "radii too small scale up");
    check(p.controlBounds().height > 4.9f, "to a semicircle");
    p.clear();
    check(parseSvgPathData("M0 0 A0 5 0 0 1 10 0", p) && p.verbs().size() == 2, "a zero radius is a line");
}

void documents() {
    check(!SvgImage::parse("").ok(), "empty");
    check(!SvgImage::parse("<html></html>").ok(), "not SVG");
    check(!SvgImage::parse("<svg viewBox=\"0 0 10 10\"><path d=\"M0 0h5\"").ok(), "truncated");
    const Result<SvgImage> doc = SvgImage::parse(
        "<?xml version=\"1.0\"?><!-- c --><svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"10\">"
        "<defs><linearGradient id=\"g\"><stop offset=\"0\"/></linearGradient></defs><title>t</title>"
        "<g fill=\"currentColor\" stroke=\"#abc\"><rect width=\"8\" height=\"8\"/><line x1=\"0\" y1=\"0\" x2=\"5\" y2=\"5\"/></g>"
        "<text>ignored</text><circle cx=\"1\" cy=\"1\" r=\"0\"/><polygon points=\"1 1 2\"/></svg>");
    check(doc.ok(), "a document with skipped parts parses");
    if (doc) {
        check(doc.value().viewBox().width == 20 && doc.value().viewBox().height == 10, "width and height without a viewBox");
        checkEqual(doc.value().shapes().size(), std::size_t{2}, "degenerate shapes are dropped");
        using Kind = SvgImage::Paint::Kind;
        check(doc.value().shapes()[0].fill.kind == Kind::CurrentColor, "fill inherits currentColor");
        check(doc.value().shapes()[1].fill.kind == Kind::None, "lines are not filled");
        const Color c = doc.value().shapes()[0].stroke.color;
        checkNear(c.r, 0xAA / 255.0, 1e-6, "#rgb colours");
        // currentColor in rendering.
        Image img = std::move(Image::create(20, 10, AlphaMode::Premultiplied).value());
        RasterPaintBackend backend(img);
        Painter painter(backend);
        doc.value().render(painter, {0, 0, 20, 10}, Color{0, 1, 0, 1});
        const std::uint8_t *px = img.pixels().data() + (2 * 20 + 5) * 4;
        check(px[1] == 255 && px[0] == 0 && px[3] == 255, "currentColor paints with the given colour");
    }
    // Hostile input: huge numbers, deep nesting, junk.
    String deep = "<svg viewBox=\"0 0 1 1\">";
    for (int i = 0; i < 2000; ++i) {
        deep += "<g transform=\"scale(1e30)\">";
    }
    deep += "<path d=\"M0 0L1e38 1e38A1e30 1e30 0 1 1 -1e38 3\"/></svg>";
    const Result<SvgImage> hostile = SvgImage::parse(deep);
    if (hostile) {
        Image img = std::move(Image::create(8, 8, AlphaMode::Premultiplied).value());
        RasterPaintBackend backend(img);
        Painter painter(backend);
        hostile.value().render(painter, {0, 0, 8, 8});
    }
    check(true, "hostile documents do not crash");
}

} // namespace

int main() {
    againstQt();
    pathData();
    documents();
    return cfw::test::finish("SvgImageTest");
}
