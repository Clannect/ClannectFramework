// Font files from anywhere: FontFace must load them or fail cleanly, and
// every lookup and outline must stay inside the data (ASan/UBSan) and
// finish (bounded work: subroutine depth, operator budget, component depth).

#include <memory>
#include <vector>

#include "FuzzTarget.h"
#include "cfw/text/FontFace.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    auto bytes = std::make_shared<const std::vector<std::byte>>(reinterpret_cast<const std::byte *>(data),
                                                                reinterpret_cast<const std::byte *>(data) + size);
    const cfw::Result<std::uint32_t> faces = cfw::FontFace::faceCount(*bytes);
    if (!faces) {
        return 0;
    }
    cfw::PainterPath path;
    for (std::uint32_t index = 0; index < faces.value() && index < 4; ++index) {
        const cfw::Result<std::shared_ptr<const cfw::FontFace>> face = cfw::FontFace::load(bytes, index);
        if (!face) {
            continue;
        }
        const cfw::FontFace &f = *face.value();
        for (const char32_t c : {U'A', U'a', U' ', U'é', U'ب', U'一', U'\U0001F600', U'￿'}) {
            (void)f.glyphIndex(c);
            (void)f.glyphIndex(c, U'️');
        }
        const std::uint32_t glyphs = f.glyphCount() < 512 ? f.glyphCount() : 512;
        for (std::uint32_t g = 0; g < glyphs; ++g) {
            (void)f.glyphOutline(static_cast<cfw::GlyphId>(g), path);
            (void)f.advanceWidth(static_cast<cfw::GlyphId>(g));
            (void)f.leftSideBearing(static_cast<cfw::GlyphId>(g));
        }
        (void)f.familyName();
        (void)f.table(cfw::FontFace::tag("GSUB"));
    }
    return 0;
}
