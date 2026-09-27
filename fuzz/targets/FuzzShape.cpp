// Shaping hostile text with hostile layout tables: the shaper must stay
// inside the font data and its buffers (ASan/UBSan) and finish (the work
// budget bounds lookups; nesting and context lengths are capped).
//
// Input: a flags byte (font, direction, script, features), for the patched
// font a count of 3-byte patches (big-endian offset, value) applied to
// CfwTestLayout.ttf's bytes, then the text as big-endian 16-bit units:
// below 0x8000 a code point, from 0x8000 an index into a palette of
// characters the test fonts shape specially.

#include <memory>
#include <vector>

#include "FuzzTarget.h"
#include "cfw/io/FileSystem.h"
#include "cfw/text/Shaper.h"
#include "cfw/text/Unicode.h"

namespace {

using namespace cfw;

FontFace::Data readFont(const char *name) {
    Result<std::vector<std::byte>> bytes = readFile(Path(CFW_FUZZ_FONTS) / name, 16u << 20);
    return std::make_shared<const std::vector<std::byte>>(bytes ? std::move(bytes.value()) : std::vector<std::byte>{});
}

struct Fonts {
    FontFace::Data layoutBytes = readFont("CfwTestLayout.ttf");
    std::shared_ptr<const FontFace> faces[3];
    Fonts() {
        const char *names[] = {"CfwTestLayout.ttf", "DejaVuSans.ttf", "CfwTestPlain.ttf"};
        for (int i = 0; i < 3; ++i) {
            if (Result<std::shared_ptr<const FontFace>> f = FontFace::load(readFont(names[i]))) {
                faces[i] = f.value();
            }
        }
    }
};

constexpr char32_t kPalette[] = {
    U'a', U'b', U'c', U'e', U'f', U'i', U'o', U'p', U'q', U'r', U's', U't', U'u', U'v', U'w', U'x', U'y', U'z',
    U'A', U'V', U'T', U' ', U'0', U'1', U'2', 0x2044, 0x0300, 0x0301, 0x0302, 0x0307, 0x0323, 0x0327, 0x0328,
    0x034F, 0x200C, 0x200D, 0x00AD, 0xFE0F, 0xE0100, 0x0627, 0x0628, 0x062A, 0x0644, 0x0645, 0x0646, 0x0647,
    0x0631, 0x064A, 0x0640, 0x064E, 0x064F, 0x0650, 0x0651, 0x0652, 0x0670, 0x0653, 0x0654, 0x0655, 0x05D0,
    0x05E9, 0x05B4, 0x05B7, 0x05BC, 0x05C1, 0x0E01, 0x0E33, 0x0E48, 0x0E4D, 0x0391, 0x03A4, 0x03B1, 0x03B2,
    0x03B3, 0x1F1E6, 0x1F1E8, 0x1F44D, 0x1F3FD, 0x2002, 0x2007, 0x202F, 0x0028, 0x0029, 0xAC00, 0x1100, 0x1161};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    static const Fonts fonts;
    static Shaper shaper; // plans persist across inputs, as in real use
    if (size < 1) {
        return 0;
    }
    const std::uint8_t flags = data[0];
    std::size_t at = 1;
    std::shared_ptr<const FontFace> face;
    const unsigned which = flags & 3;
    if (which == 3) {
        // The layout font with patched bytes.
        std::vector<std::byte> bytes = *fonts.layoutBytes;
        const unsigned patches = at < size ? data[at++] : 0;
        for (unsigned p = 0; p < patches && at + 3 <= size && !bytes.empty(); ++p, at += 3) {
            const std::size_t offset = (static_cast<std::size_t>(data[at]) << 8 | data[at + 1]) * 7 % bytes.size();
            bytes[offset] = static_cast<std::byte>(data[at + 2]);
        }
        Result<std::shared_ptr<const FontFace>> loaded = FontFace::load(std::make_shared<const std::vector<std::byte>>(std::move(bytes)));
        if (!loaded) {
            return 0;
        }
        face = loaded.value();
    } else {
        face = fonts.faces[which];
    }
    if (!face) {
        return 0;
    }
    std::vector<char32_t> text;
    for (; at + 2 <= size && text.size() < 512; at += 2) {
        const unsigned unit = static_cast<unsigned>(data[at]) << 8 | data[at + 1];
        text.push_back(unit < 0x8000 ? static_cast<char32_t>(unit) : kPalette[(unit - 0x8000) % std::size(kPalette)]);
    }
    ShapeOptions options;
    if (flags & 4) {
        options.direction = TextDirection::RightToLeft;
    }
    if (flags & 8) {
        options.language = FontFace::tag("TRK ");
    }
    static const FontFeature featureSets[4][3] = {
        {{FontFace::tag("liga"), 0}, {FontFace::tag("kern"), 0, 1, 3}, {FontFace::tag("salt"), 2}},
        {{FontFace::tag("rand"), 1}, {FontFace::tag("ss01"), 1}, {FontFace::tag("ss06"), 1, 2, 9}},
        {{FontFace::tag("frac"), 1}, {FontFace::tag("ss03"), 1}, {FontFace::tag("mark"), 0, 0, 2}},
        {{FontFace::tag("aalt"), 255}, {FontFace::tag("calt"), 0, 3, 4}, {FontFace::tag("curs"), 0}}};
    if (flags & 0x10) {
        options.features = featureSets[(flags >> 5) & 3];
    }
    if (flags & 0x80) {
        options.script = static_cast<unicode::Script>((flags >> 5) * 29 % 200);
    }
    std::vector<ShapedGlyph> out;
    shaper.shape(*face, text, options, out);
    return 0;
}
