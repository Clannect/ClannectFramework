// TextLayout and FontDatabase by their properties: widths add up, wrapped
// lines fit, hard breaks and line limits cut where they should, the
// ellipsis fits, alignment places lines, right-to-left runs come out in
// visual order, carets move monotonically and hit-testing a caret finds it
// again, fallback fonts fill in missing characters, and laying out the same
// text twice does no work. Random text at random widths must not crash.

#include "cfw/text/TextLayout.h"

#include <cmath>
#include <random>
#include <string>

#include "cfw/io/FileSystem.h"
#include "cfw/test/Check.h"
#include "cfw/text/FontDatabase.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

const Path kFonts = Path(CFW_TEXT_TESTDATA) / "fonts";

std::shared_ptr<const FontFace> load(const char *name) {
    Result<std::vector<std::byte>> bytes = readFile(kFonts / name, 64u << 20);
    const Result<std::shared_ptr<const FontFace>> f =
        FontFace::load(std::make_shared<const std::vector<std::byte>>(bytes ? std::move(bytes.value()) : std::vector<std::byte>{}));
    check(f.ok(), "font loads");
    return f ? f.value() : nullptr;
}

TextLayout make(const std::u32string &text, const TextStyle &style, const TextLayoutOptions &options = {}) {
    TextLayout layout;
    layout.setText(Span<const char32_t>(text.data(), text.size()));
    layout.layout(style, options);
    return layout;
}

void singleLine(const TextStyle &style) {
    const std::u32string text = U"Hello, world";
    TextLayout l = make(text, style);
    checkEqual(l.lines().size(), std::size_t{1}, "one line");
    const TextLayout::Line &line = l.lines()[0];
    // The width is the sum of the shaped advances at this size.
    Shaper shaper;
    std::vector<ShapedGlyph> shaped;
    ShapeOptions o;
    o.script = unicode::Script::Latin;
    shaper.shape(*style.font, Span<const char32_t>(text.data(), text.size()), o, shaped);
    float sum = 0;
    for (const ShapedGlyph &g : shaped) {
        sum += static_cast<float>(g.xAdvance) * style.pixelSize / static_cast<float>(style.font->unitsPerEm());
    }
    checkNear(line.width, sum, 1e-3, "width is the sum of advances");
    checkEqual(l.glyphs().size(), shaped.size(), "every glyph placed");
    const float ascent = static_cast<float>(style.font->lineMetrics().ascender) * style.pixelSize /
                         static_cast<float>(style.font->unitsPerEm());
    checkNear(line.baseline, ascent, 1e-3, "baseline at the ascent");
    check(l.size().y > line.ascent, "height covers the line");
    for (std::size_t i = 1; i < l.glyphs().size(); ++i) {
        check(l.glyphs()[i].x > l.glyphs()[i - 1].x, "glyphs advance left to right");
    }
}

void wrapping(const TextStyle &style) {
    const std::u32string text = U"The quick brown fox jumps over the lazy dog, again and again.";
    TextLayoutOptions o;
    o.maxWidth = 90;
    TextLayout l = make(text, style, o);
    check(l.lines().size() > 3, "wraps into several lines");
    std::uint32_t expected = 0;
    bool fits = true;
    bool noLeadingSpace = true;
    for (const TextLayout::Line &line : l.lines()) {
        checkEqual(line.start, expected, "lines are consecutive");
        expected = line.end;
        fits = fits && line.width <= o.maxWidth + 1e-3f;
        noLeadingSpace = noLeadingSpace && text[line.start] != U' ';
    }
    checkEqual(expected, static_cast<std::uint32_t>(text.size()), "lines cover the text");
    check(fits, "every line fits the width");
    check(noLeadingSpace, "lines break after spaces, not before");
    for (std::size_t i = 1; i < l.lines().size(); ++i) {
        check(l.lines()[i].top > l.lines()[i - 1].top, "lines go down");
    }
    // A word wider than the width breaks between graphemes.
    o.maxWidth = 30;
    TextLayout w = make(U"Supercalifragilistic", style, o);
    check(w.lines().size() > 2, "a long word is broken");
    for (const TextLayout::Line &line : w.lines()) {
        check(line.end > line.start, "no empty line");
    }
    // No wrapping.
    o.wrap = false;
    checkEqual(make(text, style, o).lines().size(), std::size_t{1}, "no wrap: one line");
}

void hardBreaks(const TextStyle &style) {
    TextLayout l = make(U"one\ntwo\r\n\nthree\n", style);
    checkEqual(l.lines().size(), std::size_t{5}, "a line per paragraph, and one after the final break");
    checkEqual(l.lines()[1].start, 4u, "the second line starts after the break");
    checkEqual(l.lines()[1].end, 9u, "CR LF belongs to its line");
    checkEqual(l.lines()[2].glyphCount, 0u, "an empty paragraph is an empty line");
    check(l.lines()[4].start == 16 && l.lines()[4].end == 16, "the empty last line");
    for (const TextLayout::Glyph &g : l.glyphs()) {
        check(g.cluster < 15 && l.text()[g.cluster] != U'\n' && l.text()[g.cluster] != U'\r', "no glyphs for line breaks");
    }
    TextLayout empty = make(U"", style);
    checkEqual(empty.lines().size(), std::size_t{1}, "empty text has one line");
    check(empty.caret(0).bottom > 0, "and a caret");
}

void ellipsis(const TextStyle &style) {
    const std::u32string text = U"This sentence is much too long for its box.";
    TextLayoutOptions o;
    o.maxWidth = 80;
    o.wrap = false;
    o.elide = true;
    TextLayout l = make(text, style, o);
    check(l.elided(), "elided");
    check(l.lines()[0].width <= 80.0f + 1e-3f, "the elided line fits");
    const GlyphId dots = style.font->glyphIndex(0x2026);
    check(!l.glyphs().empty() && l.glyphs().back().glyph == dots, "ends with the ellipsis");
    o.wrap = true;
    o.maxLines = 2;
    TextLayout m = make(text + U" And more text follows here.", style, o);
    checkEqual(m.lines().size(), std::size_t{2}, "two lines at most");
    check(m.elided() && m.glyphs().back().glyph == dots, "the second ends with the ellipsis");
    check(m.lines()[1].width <= 80.0f + 1e-3f, "and fits");
    o.maxLines = 0;
    o.wrap = false;
    o.maxWidth = 1000;
    check(!make(text, style, o).elided(), "text that fits is not elided");
}

void alignment(const TextStyle &style) {
    TextLayoutOptions o;
    o.maxWidth = 300;
    o.align = TextAlign::Right;
    TextLayout r = make(U"right", style, o);
    checkNear(r.lines()[0].x + r.lines()[0].width, 300.0, 1e-3, "right aligned");
    checkNear(r.glyphs()[0].x, r.lines()[0].x, 1e-3, "glyphs move with the line");
    o.align = TextAlign::Center;
    TextLayout c = make(U"centre", style, o);
    checkNear(c.lines()[0].x * 2 + c.lines()[0].width, 300.0, 1e-3, "centred");
    o.align = TextAlign::Start;
    TextLayout h = make(U"שלום", style, o);
    check(h.lines()[0].rtl, "a Hebrew paragraph is right-to-left");
    checkNear(h.lines()[0].x + h.lines()[0].width, 300.0, 1e-3, "and starts at the right");
}

void bidiAndCarets(const TextStyle &style) {
    // Left-to-right with marks: carets only at grapheme boundaries.
    const std::u32string latin = U"café au lait";
    TextLayout l = make(latin, style);
    float lastX = -1;
    bool monotonic = true;
    bool roundTrip = true;
    for (std::size_t i = 0; i <= latin.size(); i = l.nextCaret(i)) {
        const TextLayout::Caret c = l.caret(i);
        monotonic = monotonic && c.x >= lastX;
        lastX = c.x;
        roundTrip = roundTrip && l.hitTest({c.x, c.top + 1}) == i;
        if (i == latin.size()) {
            break;
        }
    }
    check(monotonic, "carets move right");
    check(roundTrip, "hit-testing a caret finds it");
    checkEqual(l.nextCaret(3), std::size_t{5}, "the combining accent is skipped");
    checkEqual(l.previousCaret(5), std::size_t{3}, "backwards too");
    checkNear(l.caret(latin.size()).x, l.lines()[0].width, 1e-3, "the end caret is at the right edge");
    check(l.hitTest({-50, 5}) == 0 && l.hitTest({1e4f, 5}) == latin.size(), "outside the text: the ends");

    // Mixed: the Hebrew word reads right to left inside left-to-right text.
    const std::u32string mixed = U"ab אבג cd";
    TextLayout m = make(mixed, style);
    check(!m.lines()[0].rtl, "the paragraph is left-to-right");
    const float x3 = m.caret(3).x; // before alef: its right edge
    const float x5 = m.caret(5).x; // before gimel
    check(x3 > x5, "within the Hebrew word carets move left");
    float alefX = 0;
    float gimelX = 0;
    for (const TextLayout::Glyph &g : m.glyphs()) {
        alefX = g.cluster == 3 ? g.x : alefX;
        gimelX = g.cluster == 5 ? g.x : gimelX;
    }
    check(gimelX < alefX, "the word's glyphs are in visual order");

    // Right-to-left paragraph: the start caret is at the right.
    const std::u32string hebrew = U"שלום עולם";
    TextLayout h = make(hebrew, style);
    checkNear(h.caret(0).x, h.lines()[0].x + h.lines()[0].width, 1e-3, "right-to-left text starts at the right");
    checkNear(h.caret(hebrew.size()).x, h.lines()[0].x, 1e-3, "and ends at the left");
    bool rtlRoundTrip = true;
    for (std::size_t i = 0; i <= hebrew.size(); ++i) {
        const TextLayout::Caret c = h.caret(i);
        rtlRoundTrip = rtlRoundTrip && h.hitTest({c.x, c.top + 1}) == i;
    }
    check(rtlRoundTrip, "right-to-left hit-testing finds each caret");

    // Multi-line carets.
    TextLayoutOptions o;
    o.maxWidth = 60;
    TextLayout w = make(U"alpha beta gamma delta", style, o);
    check(w.lines().size() > 1, "wrapped");
    const TextLayout::Line &second = w.lines()[1];
    const TextLayout::Caret c = w.caret(second.start);
    checkEqual(c.line, std::size_t{1}, "a wrapped line's start caret is on that line");
    checkEqual(w.hitTest({c.x, c.top + 1}), static_cast<std::size_t>(second.start), "and hit-tests there");
    checkEqual(w.hitTest({1e4f, 1e4f}), w.text().size(), "below and right of the text: the end");
}

void utf8(const TextStyle &style) {
    TextLayout l;
    l.setText(StringView("a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80z\xff"));
    l.layout(style);
    checkEqual(l.text().size(), std::size_t{6}, "decoded code points (invalid byte: U+FFFD)");
    checkEqual(l.utf8Offset(2), std::size_t{3}, "byte offsets");
    checkEqual(l.utf8Offset(6), std::size_t{12}, "the end");
    checkEqual(static_cast<std::uint32_t>(l.text()[5]), 0xFFFDu, "replacement character");
}

void fallbackAndDatabase() {
    FontDatabase db;
    check(db.addDirectory(kFonts) >= 6, "the test fonts register");
    const std::vector<String> families = db.families();
    check(std::find(families.begin(), families.end(), String("DejaVu Sans")) != families.end(), "DejaVu Sans is a family");
    const std::shared_ptr<const FontFace> dejavu = db.match("dejavu sans");
    check(dejavu && dejavu->familyName() == "DejaVu Sans", "matching is case-insensitive");
    check(db.match("No Such Font") == nullptr, "unknown families match nothing");
    const std::shared_ptr<const FontFace> plain = db.match("CfwTest Cff");
    check(plain != nullptr, "the CFF test font");
    const std::shared_ptr<const FontFace> hebrew = db.fallback(0x05D0, plain.get());
    check(hebrew && hebrew->glyphIndex(0x05D0) != 0, "a fallback face has the character");
    check(db.fallback(0x10FFFD) == nullptr, "nothing has a private-use plane character");
    check(db.fallback(0x05D0, plain.get()) == hebrew, "answers are cached");

    TextStyle style{plain, 16.0f, {}, &db};
    TextLayout l = make(U"abc אב xyz", style);
    check(l.fonts().size() == 2, "the layout uses a fallback font");
    bool hebrewFromFallback = false;
    for (const TextLayout::Glyph &g : l.glyphs()) {
        hebrewFromFallback = hebrewFromFallback || (g.cluster == 4 && g.font == 1 && g.glyph != 0);
    }
    check(hebrewFromFallback, "Hebrew comes from the fallback font");
    (void)db.addSystemFonts(); // whatever is installed; must not fail
}

void caching(const TextStyle &style) {
    ShapeCache cache(8);
    TextLayout l;
    const std::u32string t = U"static label";
    l.setText(Span<const char32_t>(t.data(), t.size()));
    l.layout(style, {}, &cache);
    const std::uint64_t misses = cache.misses();
    const std::uint64_t hits = cache.hits();
    for (int i = 0; i < 100; ++i) {
        l.setText(Span<const char32_t>(t.data(), t.size()));
        l.layout(style, {}, &cache);
    }
    check(cache.misses() == misses && cache.hits() == hits, "an unchanged layout does no work");
    TextLayout other;
    other.setText(Span<const char32_t>(t.data(), t.size()));
    TextStyle bigger = style;
    bigger.pixelSize = 30;
    other.layout(bigger, {}, &cache);
    check(cache.hits() > hits && cache.misses() == misses, "another size reuses the shaping");
    for (int i = 0; i < 20; ++i) {
        const std::u32string s = U"text " + std::u32string(1, static_cast<char32_t>(U'a' + static_cast<unsigned>(i)));
        other.setText(Span<const char32_t>(s.data(), s.size()));
        other.layout(style, {}, &cache);
    }
    check(cache.misses() > misses, "new text is shaped");
}

void randomText(const TextStyle &style) {
    std::mt19937 rng(5);
    const char32_t pool[] = {U'a', U'b', U' ', U'\n', U'\t', 0x0301, 0x05D0, 0x05D1, 0x0628, 0x0644, 0x0627, 0x200D, 0x200F,
                             0x202E, 0x2066, 0x2069, U'1', U'2', U'(', 0x4E00, 0xFFFF, 0x1F600, 0x1F3FD, U'\r', 0x2029, 0};
    TextLayout l;
    for (int round = 0; round < 500; ++round) {
        std::u32string t;
        const std::size_t n = rng() % 60;
        for (std::size_t i = 0; i < n; ++i) {
            t.push_back(pool[rng() % std::size(pool)]);
        }
        TextLayoutOptions o;
        o.maxWidth = static_cast<float>(rng() % 200);
        o.wrap = rng() % 2;
        o.elide = rng() % 2;
        o.maxLines = static_cast<int>(rng() % 3);
        o.align = static_cast<TextAlign>(rng() % 5);
        l.setText(Span<const char32_t>(t.data(), t.size()));
        l.layout(style, o);
        for (std::size_t i = 0; i <= t.size(); ++i) {
            const TextLayout::Caret c = l.caret(i);
            (void)l.hitTest({c.x, c.top});
            check(std::isfinite(c.x), "carets are finite");
        }
    }
}

} // namespace

int main() {
    const std::shared_ptr<const FontFace> font = load("DejaVuSans.ttf");
    if (font) {
        const TextStyle style{font, 16.0f, {}, nullptr};
        singleLine(style);
        wrapping(style);
        hardBreaks(style);
        ellipsis(style);
        alignment(style);
        bidiAndCarets(style);
        utf8(style);
        caching(style);
        randomText(style);
    }
    fallbackAndDatabase();
    return cfw::test::finish("TextLayoutTest");
}
