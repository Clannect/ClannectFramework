// JPEG decoder: photos from other creators, baseline and progressive, and
// their EXIF orientation.

#include "cfw/image/Jpeg.h"
#include "cfw/image/Orientation.h"

#include "FuzzImageLimits.h"
#include "FuzzTarget.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const cfw::ImageLimits limits = fuzzImageLimits();
    const cfw::Span<const std::byte> bytes(reinterpret_cast<const std::byte *>(data), size);
    const cfw::Result<cfw::Image> image = cfw::decodeJpeg(bytes, limits);
    if (image) {
        checkDecoded(image.value(), limits);
    }
    const std::optional<int> orientation = cfw::jpegExifOrientation(bytes);
    if (orientation && (*orientation < 1 || *orientation > 8)) {
        std::abort();
    }
    return 0;
}
