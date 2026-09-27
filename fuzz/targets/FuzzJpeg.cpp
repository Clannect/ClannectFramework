// JPEG decoder: photos from other creators, baseline and progressive.

#include "cfw/image/Jpeg.h"

#include "FuzzImageLimits.h"
#include "FuzzTarget.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const cfw::ImageLimits limits = fuzzImageLimits();
    const cfw::Result<cfw::Image> image =
        cfw::decodeJpeg(cfw::Span<const std::byte>(reinterpret_cast<const std::byte *>(data), size), limits);
    if (image) {
        checkDecoded(image.value(), limits);
    }
    return 0;
}
