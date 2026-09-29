// FontFace against values from outside references (fontTools, and FreeType
// where fontTools falls short; testing/text-oracle/make_test_fonts.py): names,
// metrics, the whole character map, and every glyph's outline (in hashed
// blocks of 64). The test fonts cover TrueType simple and composite glyphs
// (offsets, scales, 2x2, point matching, nesting), CFF name-keyed and
// CID-keyed with subroutines and every Type 2 operator, and a collection.
// Then hostile data: truncations, corruption, garbage.

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "cfw/core/Sha256.h"
#include "cfw/io/FileSystem.h"
#include "cfw/io/Json.h"
#include "cfw/io/JsonReader.h"
#include "cfw/test/Check.h"
#include "cfw/text/FontFace.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

const Path kFonts = Path(CFW_TEXT_TESTDATA) / "fonts";

FontFace::Data readFont(StringView name) {
    Result<std::vector<std::byte>> bytes = readFile(kFonts / name, 64u << 20);
    check(bytes.ok(), "test font reads");
    return std::make_shared<const std::vector<std::byte>>(bytes ? std::move(bytes.value()) : std::vector<std::byte>());
}

// The canonical outline text shared with testing/text-oracle/outlines.py.
String canonical(const PainterPath &path) {
    struct Segment {
        char type;
        std::vector<long> v;
    };
    std::vector<std::vector<Segment>> contours;
    std::vector<std::pair<long, long>> starts;
    const auto q = [](float v) { return std::lround(static_cast<double>(v) * 64.0); };
    const Span<const Vec2> pts = path.points();
    std::size_t i = 0;
    for (const PainterPath::Verb verb : path.verbs()) {
        switch (verb) {
        case PainterPath::Verb::Move:
            contours.emplace_back();
            starts.emplace_back(q(pts[i].x), q(pts[i].y));
            ++i;
            break;
        case PainterPath::Verb::Line:
            contours.back().push_back({'L', {q(pts[i].x), q(pts[i].y)}});
            ++i;
            break;
        case PainterPath::Verb::Quad:
            contours.back().push_back({'Q', {q(pts[i].x), q(pts[i].y), q(pts[i + 1].x), q(pts[i + 1].y)}});
            i += 2;
            break;
        case PainterPath::Verb::Cubic:
            contours.back().push_back({'C', {q(pts[i].x), q(pts[i].y), q(pts[i + 1].x), q(pts[i + 1].y),
                                             q(pts[i + 2].x), q(pts[i + 2].y)}});
            i += 3;
            break;
        case PainterPath::Verb::Close: {
            std::vector<Segment> &c = contours.back();
            const auto [sx, sy] = starts.back();
            const long ex = c.empty() ? sx : c.back().v[c.back().v.size() - 2];
            const long ey = c.empty() ? sy : c.back().v.back();
            if (ex != sx || ey != sy) {
                c.push_back({'L', {sx, sy}});
            }
            break;
        }
        }
    }
    String out;
    for (const std::vector<Segment> &c : contours) {
        if (c.empty()) {
            continue;
        }
        std::size_t k = 0;
        for (std::size_t j = 1; j < c.size(); ++j) {
            const auto &a = c[j].v;
            const auto &b = c[k].v;
            const long ax = a[a.size() - 2];
            const long bx = b[b.size() - 2];
            if (ax < bx || (ax == bx && a.back() < b.back())) {
                k = j;
            }
        }
        if (!out.empty()) {
            out += " | ";
        }
        for (std::size_t j = 0; j < c.size(); ++j) {
            const Segment &s = c[(k + 1 + j) % c.size()];
            if (j > 0) {
                out += ' ';
            }
            out += s.type;
            for (const long v : s.v) {
                out += ' ' + std::to_string(v);
            }
        }
    }
    return out;
}

String hash16(StringView text) { return Sha256::toHex(Sha256::hash(text)).substr(0, 16); }

void checkFont(StringView key, const JsonValue &expected) {
    String file(key);
    std::uint32_t index = 0;
    if (const std::size_t hash = file.find('#'); hash != String::npos) {
        index = static_cast<std::uint32_t>(std::stoul(file.substr(hash + 1)));
        file.resize(hash);
    }
    const FontFace::Data data = readFont(file);
    const Result<std::shared_ptr<const FontFace>> loaded = FontFace::load(data, index);
    check(loaded.ok(), "the test font loads");
    if (!loaded) {
        return;
    }
    const FontFace &face = *loaded.value();
    const String what = String(key) + ": ";
    // The database lists fonts with describe(), which must say what load() does.
    const Result<FontFace::Description> described = FontFace::describe(*data, index);
    check(described.ok() && described.value().family == face.familyName() &&
              described.value().style == face.styleName() && described.value().weight == face.weight() &&
              described.value().italic == face.isItalic(),
          (what + "describe() agrees with load()").c_str());
    checkEqual(face.glyphCount(), static_cast<std::uint32_t>(expected["glyphs"].toDouble(0)), (what + "glyph count").c_str());
    checkEqual(face.familyName(), String(expected["family"].toString("")), (what + "family name").c_str());
    checkEqual(face.styleName(), String(expected["style"].toString("")), (what + "style name").c_str());
    checkEqual(face.unitsPerEm(), static_cast<int>(expected["unitsPerEm"].toDouble(0)), (what + "units per em").c_str());
    checkEqual(face.lineMetrics().ascender, static_cast<int>(expected["ascender"].toDouble(0)), (what + "ascender").c_str());
    checkEqual(face.lineMetrics().descender, static_cast<int>(expected["descender"].toDouble(0)), (what + "descender").c_str());
    checkEqual(face.lineMetrics().lineGap, static_cast<int>(expected["lineGap"].toDouble(0)), (what + "line gap").c_str());
    checkEqual(face.weight(), static_cast<int>(expected["weight"].toDouble(0)), (what + "weight").c_str());
    checkEqual(face.hasCffOutlines(), expected["cff"].toBool(false), (what + "outline format").c_str());

    // The character map, over every code point.
    String cmap;
    for (char32_t c = 0; c <= 0x10FFFF; ++c) {
        if (const GlyphId g = face.glyphIndex(c)) {
            char line[32];
            std::snprintf(line, sizeof line, "%X %u\n", static_cast<unsigned>(c), static_cast<unsigned>(g));
            cmap += line;
        }
    }
    checkEqual(hash16(cmap), String(expected["cmap"].toString("")), (what + "character map").c_str());

    // Every outline, advance and side bearing, hashed per 64 glyphs.
    const JsonArray *blocks = expected["blocks"].asArray();
    check(blocks != nullptr, "expected outline blocks");
    PainterPath path;
    std::size_t failures = 0;
    for (std::uint32_t b = 0; blocks != nullptr && b < blocks->size(); ++b) {
        String text;
        for (std::uint32_t g = b * 64; g < std::min(face.glyphCount(), (b + 1) * 64); ++g) {
            const bool ok = face.glyphOutline(static_cast<GlyphId>(g), path);
            text += std::to_string(g) + ' ' + std::to_string(face.advanceWidth(static_cast<GlyphId>(g))) + ' ' +
                    std::to_string(face.leftSideBearing(static_cast<GlyphId>(g))) + ' ' + (ok ? "" : "FAILED ") +
                    canonical(path) + '\n';
        }
        if (hash16(text) != (*blocks)[b].toString("")) {
            if (++failures <= 3) {
                std::printf("  %s glyphs %u-%u differ from the reference\n", String(key).c_str(), b * 64, b * 64 + 63);
            }
        }
    }
    checkEqual(failures, std::size_t{0}, (what + "every outline matches the reference").c_str());
}

void loading() {
    const FontFace::Data collection = readFont("CfwTestCollection.ttc");
    checkEqual(FontFace::faceCount(*collection).valueOr(0), 2u, "a collection has its faces");
    check(!FontFace::load(collection, 2).ok(), "a face index beyond the collection fails");
    const std::vector<std::byte> junk(100, std::byte{0x41});
    check(!FontFace::faceCount(junk).ok(), "not a font");
    check(!FontFace::load(std::make_shared<const std::vector<std::byte>>(), 0).ok(), "empty data");

    const Result<std::shared_ptr<const FontFace>> face = FontFace::load(readFont("DejaVuSans.ttf"));
    check(face.ok(), "DejaVu Sans loads");
    if (!face) {
        return;
    }
    const FontFace &f = *face.value();
    check(f.glyphIndex(U'A') != 0 && f.glyphIndex(U'ب') != 0, "Latin and Arabic are mapped");
    checkEqual(f.glyphIndex(U'\U000F0000'), GlyphId{0}, "unmapped characters give .notdef");
    checkEqual(f.glyphIndex(U'A', U'️'), f.glyphIndex(U'A'), "an unknown variation sequence falls back");
    PainterPath p;
    check(f.glyphOutline(f.glyphIndex(U' '), p) && p.empty(), "a space has an empty outline");
    check(!f.glyphOutline(60000, p), "a glyph beyond the font does not exist");
    check(!f.table(FontFace::tag("GSUB")).empty() && f.table(FontFace::tag("zzzz")).empty(), "tables");
    check(f.underlineThickness() > 0 && f.winAscent() > 0, "decoration and Windows metrics");
}

// Truncations and corruption must fail cleanly or give (possibly wrong)
// outlines; never read outside the data (checked by ASan/UBSan builds).
void hostileData() {
    for (const char *name : {"CfwTestCff.otf", "CfwTestComposite.ttf", "CfwTestCid.otf"}) {
        const FontFace::Data good = readFont(name);
        PainterPath p;
        int describeMissed = 0;
        std::size_t loaded = 0;
        for (std::size_t length = 0; length < good->size(); length += 97) {
            auto cut = std::make_shared<const std::vector<std::byte>>(good->begin(),
                                                                        good->begin() + static_cast<std::ptrdiff_t>(length));
            const Result<FontFace::Description> described = FontFace::describe(*cut);
            if (const Result<std::shared_ptr<const FontFace>> f = FontFace::load(cut)) {
                ++loaded;
                describeMissed += described.ok() ? 0 : 1;
                for (GlyphId g = 0; g < f.value()->glyphCount(); ++g) {
                    (void)f.value()->glyphOutline(g, p);
                    (void)f.value()->advanceWidth(g);
                }
                (void)f.value()->glyphIndex(U'A');
            }
        }
        std::mt19937 rng(7);
        for (int round = 0; round < 60; ++round) {
            std::vector<std::byte> bad = *good;
            for (int k = 0; k < 20; ++k) {
                bad[rng() % bad.size()] = static_cast<std::byte>(rng());
            }
            const auto badData = std::make_shared<const std::vector<std::byte>>(std::move(bad));
            const Result<FontFace::Description> described = FontFace::describe(*badData);
            if (const Result<std::shared_ptr<const FontFace>> f = FontFace::load(badData)) {
                describeMissed += described.ok() ? 0 : 1;
                for (GlyphId g = 0; g < f.value()->glyphCount(); ++g) {
                    (void)f.value()->glyphOutline(g, p);
                }
                for (char32_t c = 0; c < 0x300; ++c) {
                    (void)f.value()->glyphIndex(c);
                }
            }
        }
        check(true, "truncated and corrupted fonts do not crash");
        checkEqual(describeMissed, 0, "describe() accepts every font load() does");
        (void)loaded;
    }
}

} // namespace

int main() {
    loading();
    const Result<std::vector<std::byte>> json = readFile(kFonts / "expected.json");
    check(json.ok(), "expected values read");
    if (json) {
        const Result<JsonValue> expected =
            parseJson(StringView(reinterpret_cast<const char *>(json.value().data()), json.value().size()));
        check(expected.ok(), "expected values parse");
        if (expected) {
            for (const auto &[key, value] : *expected.value().asObject()) {
                checkFont(key, value);
            }
        }
    }
    hostileData();
    return cfw::test::finish("FontFaceTest");
}
