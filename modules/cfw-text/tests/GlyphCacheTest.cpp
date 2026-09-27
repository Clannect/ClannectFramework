// Glyph masks against FreeType's unhinted rendering (testdata/glyphs, from
// testing/text-oracle/glyph_masks.py): every glyph of a sample in TrueType
// and CFF fonts, six sizes, four sub-pixel offsets. Coverage is the same
// signed-area technique, so pixels must agree closely (TrueType: no pixel
// off by more than 24/255, 1% of the ink in total; CFF, where FreeType
// rounds scaled points to 1/64 pixel: 56/255 and 2.5%). Then the cache itself: hits, pages, positions and limits.

#include "cfw/text/GlyphCache.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

#include "cfw/core/Deflate.h"
#include "cfw/io/FileSystem.h"
#include "cfw/io/Json.h"
#include "cfw/io/JsonReader.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

const Path kTestData = Path(CFW_TEXT_TESTDATA);

std::shared_ptr<const FontFace> loadFont(StringView name) {
    Result<std::vector<std::byte>> bytes = readFile(kTestData / "fonts" / name, 64u << 20);
    check(bytes.ok(), "the font reads");
    const Result<std::shared_ptr<const FontFace>> face =
        FontFace::load(std::make_shared<const std::vector<std::byte>>(bytes ? std::move(bytes.value()) : std::vector<std::byte>{}));
    check(face.ok(), "the font loads");
    return face ? face.value() : nullptr;
}

void againstFreeType() {
    const Result<std::vector<std::byte>> file = readFile(kTestData / "glyphs" / "freetype-masks.json.z");
    check(file.ok(), "the reference reads");
    const Result<std::vector<std::byte>> json = file ? zlibDecompress(file.value()) : Result<std::vector<std::byte>>(file.error());
    check(json.ok(), "and decompresses");
    const Result<JsonValue> ref = json ? parseJson(StringView(reinterpret_cast<const char *>(json.value().data()), json.value().size()))
                                       : Result<JsonValue>(json.error());
    check(ref.ok(), "and parses");
    if (!ref) {
        return;
    }
    std::vector<std::uint8_t> mine;
    for (const auto &[fontName, entries] : *ref.value().asObject()) {
        const std::shared_ptr<const FontFace> face = loadFont(fontName);
        if (!face) {
            continue;
        }
        int worst = 0;
        double totalDiff = 0;
        double totalInk = 0;
        std::size_t glyphs = 0;
        std::size_t badBoxes = 0;
        for (std::size_t i = 0; i < entries.asArray()->size(); ++i) {
            const JsonValue &e = entries[i];
            const auto glyph = static_cast<GlyphId>(e[0].toDouble(0));
            const auto size = static_cast<float>(e[1].toDouble(0));
            const auto sub = static_cast<float>(e[2].toDouble(0));
            const int ftLeft = static_cast<int>(e[3].toDouble(0));
            const int ftTop = static_cast<int>(e[4].toDouble(0));
            const int ftW = static_cast<int>(e[5].toDouble(0));
            const int ftH = static_cast<int>(e[6].toDouble(0));
            const StringView hex = e[7].toString("");
            GlyphMask box;
            check(GlyphCache::render(*face, glyph, size, sub, box, mine), "the glyph renders");
            ++glyphs;
            // Compare over the union of both boxes (the boxes themselves may
            // differ by an empty row or column).
            const int x0 = std::min<int>(box.left, ftLeft);
            const int y0 = std::min<int>(box.top, ftTop);
            const int x1 = std::max<int>(box.left + box.width, ftLeft + ftW);
            const int y1 = std::max<int>(box.top + box.height, ftTop + ftH);
            const auto at = [](const std::uint8_t *px, int left, int top, int w, int h, int x, int y) -> int {
                x -= left;
                y -= top;
                return x >= 0 && y >= 0 && x < w && y < h ? px[y * w + x] : 0;
            };
            std::vector<std::uint8_t> ft(hex.size() / 2);
            for (std::size_t k = 0; k < ft.size(); ++k) {
                ft[k] = static_cast<std::uint8_t>(std::stoi(String(hex.substr(k * 2, 2)), nullptr, 16));
            }
            int glyphWorst = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const int a = at(mine.data(), box.left, box.top, box.width, box.height, x, y);
                    const int b = at(ft.data(), ftLeft, ftTop, ftW, ftH, x, y);
                    glyphWorst = std::max(glyphWorst, std::abs(a - b));
                    totalDiff += std::abs(a - b);
                    totalInk += b;
                }
            }
            worst = std::max(worst, glyphWorst);
            if (std::abs(box.left - ftLeft) > 1 || std::abs(box.top - ftTop) > 1) {
                ++badBoxes;
            }
        }
        std::printf("  %s: %zu masks, worst pixel %d, mean difference %.3f%% of ink\n", String(fontName).c_str(), glyphs,
                    worst, 100.0 * totalDiff / std::max(totalInk, 1.0));
        checkEqual(badBoxes, std::size_t{0}, "mask boxes are where FreeType puts them");
        // FreeType's CFF engine scales points in 16.16 and rounds them to
        // 1/64 pixel, which shifts small CFF glyphs slightly; TrueType agrees
        // more closely.
        const bool cff = face->hasCffOutlines();
        check(worst <= (cff ? 56 : 24), "no pixel differs by more than the tolerance");
        check(totalDiff <= totalInk * (cff ? 0.025 : 0.01), "total difference within the tolerance of the ink");
    }
}

void cache() {
    const std::shared_ptr<const FontFace> face = loadFont("DejaVuSans.ttf");
    if (!face) {
        return;
    }
    GlyphCache cache(256);
    const GlyphId a = face->glyphIndex(U'a');
    const GlyphMask *m1 = cache.glyph(*face, a, 16, 0.0f);
    check(m1 != nullptr && m1->width > 0 && m1->height > 0, "a glyph is cached");
    const GlyphMask *m2 = cache.glyph(*face, a, 16, 0.02f);
    check(m1 == m2, "offsets quantise to quarter pixels");
    const GlyphMask *m3 = cache.glyph(*face, a, 16, 0.3f);
    check(m3 != nullptr && m3 != m1, "another quarter is another mask");
    checkEqual(cache.hits(), std::uint64_t{1}, "one hit");
    checkEqual(cache.misses(), std::uint64_t{2}, "two misses");
    const GlyphMask *space = cache.glyph(*face, face->glyphIndex(U' '), 16, 0);
    check(space != nullptr && space->width == 0, "a space has an empty mask");
    // The pixels in the page are the rendered glyph.
    GlyphMask box;
    std::vector<std::uint8_t> pixels;
    check(GlyphCache::render(*face, a, 16, 0, box, pixels), "render");
    bool same = box.width == m1->width && box.height == m1->height;
    for (int y = 0; same && y < box.height; ++y) {
        for (int x = 0; x < box.width; ++x) {
            same = same && cache.page(m1->page)[static_cast<std::size_t>((m1->y + y) * cache.pageSize() + m1->x + x)] ==
                               pixels[static_cast<std::size_t>(y * box.width + x)];
        }
    }
    check(same, "the atlas holds the mask");
    // Filling pages: more glyphs than one 256x256 page holds.
    for (GlyphId g = 1; g < 400; ++g) {
        (void)cache.glyph(*face, g, 20, 0);
    }
    check(cache.pageCount() > 1, "new pages when one is full");
    check(cache.glyph(*face, a, 200, 0) == nullptr, "glyphs larger than a quarter page are not cached");
    check(cache.glyph(*face, a, -3, 0) == nullptr && cache.glyph(*face, a, std::nanf(""), 0) == nullptr, "bad sizes");
    check(cache.glyph(*face, 60000, 12, 0) == nullptr, "missing glyphs");
    cache.clear();
    checkEqual(cache.pageCount(), std::size_t{0}, "clear empties the atlas");
}

} // namespace

int main() {
    againstFreeType();
    cache();
    return cfw::test::finish("GlyphCacheTest");
}
