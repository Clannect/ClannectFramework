// SVG documents from anywhere: SvgImage must parse them or fail cleanly, and
// rendering what it parsed must stay inside the target (ASan/UBSan) and
// finish.

#include <vector>

#include "FuzzTarget.h"
#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/gfx/SvgImage.h"
#include "cfw/image/Image.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const cfw::StringView text(reinterpret_cast<const char *>(data), size);
    const cfw::Result<cfw::SvgImage> svg = cfw::SvgImage::parse(text);
    cfw::PainterPath path;
    (void)cfw::parseSvgPathData(text, path);
    if (!svg) {
        return 0;
    }
    static cfw::Image target = std::move(cfw::Image::create(24, 24, cfw::AlphaMode::Premultiplied).value());
    cfw::RasterPaintBackend backend(target);
    cfw::Painter painter(backend);
    svg.value().render(painter, {2, 2, 20, 20}, cfw::Color{0.2f, 0.4f, 0.6f, 1});
    return 0;
}
