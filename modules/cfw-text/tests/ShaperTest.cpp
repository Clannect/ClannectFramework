// Shaping, glyph for glyph against HarfBuzz: testdata/shaping/*.json.z hold
// HarfBuzz's output (testing/text-oracle/shape.py) for a corpus of Latin,
// Greek, Cyrillic, Arabic, Hebrew, Thai and other text, with marks,
// ligatures, kerning, spaces, ignorables, variation selectors, fractions and
// features, plus random strings, on four fonts: DejaVu Sans (real-world
// GSUB/GPOS), CfwTestLayout (every lookup type and format), and CfwTestPlain
// as TrueType and CFF (no layout tables: fallback mark placement, Arabic and
// Hebrew presentation forms, the kern table), and CfwTestIndic(Old), whose
// Devanagari and Malayalam lookups reach the Indic engine's context rules
// under both specs' tags. Every glyph id, cluster, advance and offset must
// match. Hostile fonts and text must not crash.
//
// Run as ShaperTest <expected.json[.z]> <font> to compare another font.

#include "cfw/text/Shaper.h"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "cfw/core/Deflate.h"
#include "cfw/io/FileSystem.h"
#include "cfw/io/Json.h"
#include "cfw/io/JsonReader.h"
#include "cfw/test/Check.h"
#include "cfw/text/Unicode.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

const Path kTestData = Path(CFW_TEXT_TESTDATA);

FontFace::Data readData(const Path &path) {
    Result<std::vector<std::byte>> bytes = readFile(path, 64u << 20);
    check(bytes.ok(), "the file reads");
    return std::make_shared<const std::vector<std::byte>>(bytes ? std::move(bytes.value()) : std::vector<std::byte>{});
}

String describe(const std::vector<ShapedGlyph> &glyphs) {
    String s;
    for (const ShapedGlyph &g : glyphs) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "[%u %u %d %d %d %d]", static_cast<unsigned>(g.glyph), g.cluster, g.xAdvance,
                      g.yAdvance, g.xOffset, g.yOffset);
        s += buf;
    }
    return s;
}

void compareWithHarfBuzz(const Path &expectedPath, const Path &fontPath) {
    const Result<std::shared_ptr<const FontFace>> loaded = FontFace::load(readData(fontPath));
    check(loaded.ok(), "the font loads");
    FontFace::Data json = readData(expectedPath);
    if (expectedPath.toString().ends_with(".z")) {
        Result<std::vector<std::byte>> inflated = zlibDecompress(*json);
        check(inflated.ok(), "the expected results decompress");
        json = std::make_shared<const std::vector<std::byte>>(inflated ? std::move(inflated.value()) : std::vector<std::byte>{});
    }
    const Result<JsonValue> expected = parseJson(StringView(reinterpret_cast<const char *>(json->data()), json->size()));
    check(expected.ok(), "the expected results parse");
    if (!loaded || !expected) {
        return;
    }
    const FontFace &face = *loaded.value();
    const JsonArray *cases = expected.value()["cases"].asArray();
    check(cases != nullptr && cases->size() > 0, "there are cases");
    Shaper shaper;
    std::vector<ShapedGlyph> out;
    std::size_t failures = 0;
    for (std::size_t i = 0; cases != nullptr && i < cases->size(); ++i) {
        const JsonValue &c = (*cases)[i];
        std::vector<char32_t> text;
        for (std::size_t k = 0; k < c["text"].asArray()->size(); ++k) {
            text.push_back(static_cast<char32_t>(c["text"][k].toDouble(0)));
        }
        std::vector<FontFeature> features;
        for (std::size_t k = 0; c["features"].asArray() && k < c["features"].asArray()->size(); ++k) {
            const JsonValue &f = c["features"][k];
            const String t(f[0].toString("    "));
            features.push_back({FontFace::tag(*reinterpret_cast<const char(*)[5]>(t.c_str())),
                                static_cast<std::uint32_t>(f[1].toDouble(1)), static_cast<std::uint32_t>(f[2].toDouble(0)),
                                static_cast<std::uint32_t>(f[3].toDouble(0))});
        }
        ShapeOptions options;
        const String script(c["script"].toString("Zyyy"));
        options.script = unicode::scriptFromIso15924(script).value_or(unicode::Script::Common);
        options.direction = c["direction"].toString("ltr") == "rtl" ? TextDirection::RightToLeft : TextDirection::LeftToRight;
        options.features = features;
        if (const String *language = c["language"].asString()) {
            String t = *language;
            t.resize(4, ' ');
            options.language = FontFace::tag(*reinterpret_cast<const char(*)[5]>(t.c_str()));
        }
        shaper.shape(face, text, options, out);

        std::vector<ShapedGlyph> want;
        const JsonArray *glyphs = c["glyphs"].asArray();
        for (std::size_t k = 0; glyphs != nullptr && k < glyphs->size(); ++k) {
            const JsonValue &g = (*glyphs)[k];
            want.push_back({static_cast<GlyphId>(g[0].toDouble(0)), static_cast<std::uint32_t>(g[1].toDouble(0)),
                            static_cast<std::int32_t>(g[2].toDouble(0)), static_cast<std::int32_t>(g[3].toDouble(0)),
                            static_cast<std::int32_t>(g[4].toDouble(0)), static_cast<std::int32_t>(g[5].toDouble(0))});
        }
        const String got = describe(out);
        const String wanted = describe(want);
        if (got != wanted) {
            if (++failures <= 20) {
                String codes;
                for (const char32_t u : text) {
                    char buf[12];
                    std::snprintf(buf, sizeof buf, " %04X", static_cast<unsigned>(u));
                    codes += buf;
                }
                std::printf("  case %zu (%s%s):\n    got  %s\n    want %s\n", i, script.c_str(), codes.c_str(), got.c_str(),
                            wanted.c_str());
            }
        }
    }
    std::printf("  %s: %zu of %zu cases differ\n", fontPath.fileName().c_str(), failures, cases ? cases->size() : 0);
    checkEqual(failures, std::size_t{0}, "every case matches HarfBuzz");
}

// Hostile input: random code points, huge runs, odd features; nothing may
// crash or run away (checked under ASan/UBSan).
void hostileInput() {
    const Result<std::shared_ptr<const FontFace>> face = FontFace::load(readData(kTestData / "fonts" / "DejaVuSans.ttf"));
    if (!face) {
        return;
    }
    Shaper shaper;
    std::vector<ShapedGlyph> out;
    std::mt19937 rng(3);
    std::vector<char32_t> text;
    for (int round = 0; round < 300; ++round) {
        text.clear();
        const std::size_t n = rng() % 40;
        for (std::size_t i = 0; i < n; ++i) {
            const unsigned kind = rng() % 4;
            text.push_back(kind == 0   ? static_cast<char32_t>(rng() % 0x110000)
                           : kind == 1 ? static_cast<char32_t>(0x300 + rng() % 0x70)
                           : kind == 2 ? static_cast<char32_t>(0x600 + rng() % 0x100)
                                       : static_cast<char32_t>(0x20 + rng() % 0x5F));
        }
        ShapeOptions options;
        if (round % 3 == 1) {
            options.direction = TextDirection::RightToLeft;
        }
        shaper.shape(*face.value(), text, options, out);
        check(out.size() <= text.size() * 4 + 4, "shaping stays bounded");
    }
    text.assign(100000, U'́');
    shaper.shape(*face.value(), text, {}, out);
    check(out.size() == text.size(), "a long run of marks shapes");
    text.assign(0, U'a');
    shaper.shape(*face.value(), text, {}, out);
    check(out.empty(), "empty text gives no glyphs");
}

// Corrupted and truncated layout tables: shaping must stay in bounds and
// finish (the work budget stops runaway lookups).
void hostileFonts() {
    const FontFace::Data good = readData(kTestData / "fonts" / "CfwTestLayout.ttf");
    Shaper shaper;
    std::vector<ShapedGlyph> out;
    const std::u32string texts[] = {U"office sop zxz yffy AVAT a\u0301\u0300 fi\u0301 beb ceg", U"\u0628\u062a\u0644\u0627\u064e\u0645",
                                    U"\u0391\u03a4\u03b2\u03b1 \u03b3\u03b4"};
    std::mt19937 rng(11);
    std::size_t shaped = 0;
    for (int round = 0; round < 400; ++round) {
        std::vector<std::byte> bad = *good;
        if (round % 4 == 0) {
            bad.resize(rng() % bad.size());
        } else {
            for (int k = 0; k < 1 + round % 30; ++k) {
                bad[rng() % bad.size()] = static_cast<std::byte>(rng());
            }
        }
        const Result<std::shared_ptr<const FontFace>> face = FontFace::load(std::make_shared<const std::vector<std::byte>>(std::move(bad)));
        if (!face) {
            continue;
        }
        for (const std::u32string &t : texts) {
            ShapeOptions options;
            options.direction = round % 2 ? TextDirection::RightToLeft : TextDirection::LeftToRight;
            shaper.shape(*face.value(), Span<const char32_t>(t.data(), t.size()), options, out);
            ++shaped;
        }
    }
    check(shaped > 0, "corrupted fonts load and shape without crashing");
}

// The canonical composition data behind normalisation.
void normalisationData() {
    char32_t a = 0;
    char32_t b = 0;
    char32_t ab = 0;
    check(unicode::decompose(U'\u00e9', a, b) && a == U'e' && b == U'\u0301', "e acute decomposes");
    check(unicode::decompose(U'\u1e69', a, b) && a == U'\u1e63' && b == U'\u0307', "one step at a time");
    check(unicode::decompose(U'\u212b', a, b) && a == U'\u00c5' && b == 0, "singletons");
    check(unicode::decompose(U'\uac01', a, b) && a == U'\uac00' && b == U'\u11a8', "Hangul LVT");
    check(unicode::decompose(U'\uac00', a, b) && a == U'\u1100' && b == U'\u1161', "Hangul LV");
    check(!unicode::decompose(U'a', a, b), "no decomposition");
    check(unicode::compose(U'e', U'\u0301', ab) && ab == U'\u00e9', "e + acute composes");
    check(unicode::compose(U'\u1100', U'\u1161', ab) && ab == U'\uac00', "Hangul L + V");
    check(unicode::compose(U'\uac00', U'\u11a8', ab) && ab == U'\uac01', "Hangul LV + T");
    check(!unicode::compose(U'\u0915', U'\u093c', ab), "composition exclusions stay apart");
    check(!unicode::compose(U'\u212b', U'a', ab), "no composition");
    // Every primary composite round-trips.
    std::size_t bad = 0;
    for (char32_t c = 0; c < 0x30000; ++c) {
        if (unicode::decompose(c, a, b) && b != 0 && unicode::compose(a, b, ab) && ab != c) {
            ++bad;
        }
    }
    checkEqual(bad, std::size_t{0}, "decompositions compose back to the same character");
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 3) {
        compareWithHarfBuzz(Path(argv[1]), Path(argv[2]));
    } else {
        const Path shaping = kTestData / "shaping";
        const Path fonts = kTestData / "fonts";
        compareWithHarfBuzz(shaping / "DejaVuSans.json.z", fonts / "DejaVuSans.ttf");
        compareWithHarfBuzz(shaping / "CfwTestLayout.json.z", fonts / "CfwTestLayout.ttf");
        compareWithHarfBuzz(shaping / "CfwTestPlain.ttf.json.z", fonts / "CfwTestPlain.ttf");
        compareWithHarfBuzz(shaping / "CfwTestPlain.otf.json.z", fonts / "CfwTestPlain.otf");
        compareWithHarfBuzz(shaping / "CfwTestIndic.json.z", fonts / "CfwTestIndic.ttf");
        compareWithHarfBuzz(shaping / "CfwTestIndicOld.json.z", fonts / "CfwTestIndicOld.ttf");
        normalisationData();
        hostileInput();
        hostileFonts();
    }
    return cfw::test::finish("ShaperTest");
}
