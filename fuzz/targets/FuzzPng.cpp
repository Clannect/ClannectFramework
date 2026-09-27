// PNG decoder: images from other creators. Whatever decodes must respect
// the limits, and re-encode and decode to the same pixels.

#include "cfw/image/Png.h"

#include <algorithm>

#include "FuzzImageLimits.h"
#include "FuzzTarget.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const cfw::ImageLimits limits = fuzzImageLimits();
    const cfw::Result<cfw::Image> image =
        cfw::decodePng(cfw::Span<const std::byte>(reinterpret_cast<const std::byte *>(data), size), limits);
    if (!image) {
        return 0;
    }
    checkDecoded(image.value(), limits);
    const cfw::Result<cfw::Image> again = cfw::decodePng(cfw::encodePng(image.value(), {1}).value(), limits);
    if (!again || !std::equal(again.value().pixels().begin(), again.value().pixels().end(),
                              image.value().pixels().begin(), image.value().pixels().end())) {
        std::abort();
    }
    return 0;
}
