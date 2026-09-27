// Drawing text: glyph masks (under a translation) and outlines (under any
// other transform) must agree; ink scales with the transform; colour,
// opacity and clipping apply to text as to any fill; drawText and
// drawGlyphs draw the same pixels; and a warm glyph cache adds no masks.

#include <cmath>
#include <cstdio>
#include <vector>

#include "cfw/gfx/Painter.h"
#include "cfw/image/Image.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/io/FileSystem.h"
#include "cfw/test/Check.h"
#include "cfw/text/TextLayout.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

std::shared_ptr<const FontFace> loadFont() {
    Result<std::vector<std::byte>> bytes = readFile(Path(CFW_TEXT_FONTS) / "DejaVuSans.ttf", 64u << 20);
    const Result<std::shared_ptr<const FontFace>> f =
        FontFace::load(std::make_shared<const std::vector<std::byte>>(bytes ? std::move(bytes.value()) : std::vector<std::byte>{}));
    check(f.ok(), "font loads");
    return f ? f.value() : nullptr;
}

Image blank(int w, int h) {
    Result<Image> img = Image::create(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), AlphaMode::Premultiplied);
    check(img.ok(), "image");
    return img ? std::move(img.value()) : Image();
}

double ink(const Image &img) {
    double sum = 0;
    const Span<const std::uint8_t> p = img.pixels();
    for (std::size_t i = 3; i < p.size(); i += 4) {
        sum += p[i];
    }
    return sum / 255.0;
}

double totalDifference(const Image &a, const Image &b) {
    double sum = 0;
    for (std::size_t i = 3; i < a.pixels().size(); i += 4) {
        sum += std::abs(int(a.pixels()[i]) - int(b.pixels()[i]));
    }
    return sum / 255.0;
}

int maxDifference(const Image &a, const Image &b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.pixels().size(); ++i) {
        worst = std::max(worst, std::abs(int(a.pixels()[i]) - int(b.pixels()[i])));
    }
    return worst;
}

TextLayout layoutOf(const std::shared_ptr<const FontFace> &font, const std::u32string &text, float size) {
    TextLayout l;
    l.setText(Span<const char32_t>(text.data(), text.size()));
    l.layout({font, size, {}, nullptr});
    return l;
}

} // namespace

int main() {
    const std::shared_ptr<const FontFace> font = loadFont();
    if (!font) {
        return cfw::test::finish("TextPaintTest");
    }
    const Brush black(Color{0, 0, 0, 1});
    const TextLayout text = layoutOf(font, U"Hamburgefonstiv 0123 éß", 18.0f);

    // Masks (translation) against outlines (a transform that is not one).
    // Masks sit on whole-pixel baselines, so put the baseline on one.
    const float b = text.lines()[0].baseline;
    const float y0 = 7.0f + (std::round(b) - b);
    // Glyphs on quarter pixels, where masks are exact.
    std::vector<Painter::PositionedGlyph> quarter;
    for (const TextLayout::Glyph &g : text.glyphs()) {
        quarter.push_back({g.glyph, {std::round(g.x * 4) / 4, g.y}});
    }
    Image masks = blank(260, 40);
    {
        RasterPaintBackend backend(masks);
        Painter p(backend);
        p.translate(3.25f, 2.0f);
        p.drawGlyphs(*font, 18.0f, quarter, {4.0f, y0 - 2.0f}, black);
    }
    Image outlines = blank(260, 40);
    {
        RasterPaintBackend backend(outlines);
        Painter p(backend);
        p.setTransform(Transform2D::translation(7.25, y0).then(Transform2D::scaling(1.0, 1.0000001)));
        p.drawGlyphs(*font, 18.0f, quarter, {0.0f, 0.0f}, black);
    }
    check(ink(masks) > 100, "text leaves ink");
    const int diff = maxDifference(masks, outlines);
    const double mean = totalDifference(masks, outlines) / ink(masks);
    std::printf("  masks vs outlines: worst %d, difference %.2f%% of the ink, ink %.1f vs %.1f\n", diff, mean * 100,
                ink(masks), ink(outlines));
    check(diff <= 24, "masks and outlines agree pixel by pixel");
    check(mean < 0.03, "and overall (curves flatten at 0.1 px as fills, 0.02 px as glyph masks)");
    check(std::abs(ink(masks) - ink(outlines)) < ink(masks) * 0.005, "and have the same ink");

    // Scaled 2x: four times the ink.
    Image scaled = blank(520, 80);
    {
        RasterPaintBackend backend(scaled);
        Painter p(backend);
        p.scale(2, 2);
        p.drawGlyphs(*font, 18.0f, quarter, {3.0f, y0 - 2.0f}, black);
    }
    const double ratio = ink(scaled) / ink(masks);
    check(std::abs(ratio - 4.0) < 0.1, "ink scales with the transform");

    // Colour, opacity, clipping.
    Image red = blank(260, 40);
    {
        RasterPaintBackend backend(red);
        Painter p(backend);
        p.setOpacity(0.5f);
        p.clipRect({0, 0, 60, 40});
        p.drawText(text, {7.25f, 7.0f}, Brush(Color{1, 0, 0, 1}));
    }
    bool redOnly = true;
    bool clipped = true;
    int maxAlpha = 0;
    for (std::uint32_t y = 0; y < 40; ++y) {
        for (std::uint32_t x = 0; x < 260; ++x) {
            const std::uint8_t *px = red.pixels().data() + (y * 260 + x) * 4;
            redOnly = redOnly && px[1] == 0 && px[2] == 0 && px[0] == px[3];
            clipped = clipped && (x < 60 || px[3] == 0);
            maxAlpha = std::max<int>(maxAlpha, px[3]);
        }
    }
    check(redOnly, "text takes the brush colour (premultiplied)");
    check(clipped, "and the clip");
    check(maxAlpha > 100 && maxAlpha <= 128, "and the opacity");

    // drawGlyphs draws what drawText draws.
    Image glyphs = blank(260, 40);
    {
        RasterPaintBackend backend(glyphs);
        Painter p(backend);
        std::vector<Painter::PositionedGlyph> run;
        for (const TextLayout::Glyph &g : text.glyphs()) {
            run.push_back({g.glyph, {g.x, g.y}});
        }
        p.drawGlyphs(*font, 18.0f, run, {7.25f, y0}, black);
    }
    Image viaText = blank(260, 40);
    {
        RasterPaintBackend backend(viaText);
        Painter p(backend);
        p.drawText(text, {7.25f, y0}, black);
    }
    checkEqual(maxDifference(glyphs, viaText), 0, "drawGlyphs matches drawText");

    // A warm cache: drawing again adds no glyphs.
    GlyphCache cache;
    Image again = blank(260, 40);
    RasterPaintBackend backend(again);
    Painter p(backend);
    p.setGlyphCache(&cache);
    p.drawText(text, {7.25f, 7.0f}, black);
    const std::size_t glyphCount = cache.glyphCount();
    const std::uint64_t misses = cache.misses();
    for (int i = 0; i < 10; ++i) {
        p.drawText(text, {7.25f, 7.0f}, black);
    }
    check(cache.glyphCount() == glyphCount && cache.misses() == misses, "a warm cache renders nothing new");
    check(cache.hits() >= 10 * text.glyphs().size(), "and hits every glyph");

    // Hostile positions and sizes do nothing harmful.
    p.drawText(text, {std::nanf(""), 1e30f}, black);
    p.drawGlyphs(*font, -5, {}, {0, 0}, black);
    p.drawGlyphs(*font, 1e6f, std::vector<Painter::PositionedGlyph>{{font->glyphIndex(U'A'), {0, 0}}}, {0, 0}, black);
    return cfw::test::finish("TextPaintTest");
}
