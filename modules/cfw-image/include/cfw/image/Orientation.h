#pragma once

// EXIF orientation: cameras store a photo as the sensor saw it and record,
// in the EXIF block, how it should be turned for display. Qt applied this
// when reading (QImageReader::setAutoTransform); decodeImage() applies it
// when asked (ImageOrientation::ApplyExif).
//
// Orientation values are the EXIF/TIFF ones:
//   1 as stored          2 mirrored left-right   3 turned 180 degrees
//   4 mirrored top-bottom 5 transposed (mirrored along the main diagonal)
//   6 turned 90 degrees clockwise   7 transverse   8 turned 90 degrees anticlockwise
//
// Threads: any (pure functions). Allocates: orientImage() the new image.

#include <cstddef>
#include <optional>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/image/Image.h"
#include "cfw/image/ImageLimits.h"

namespace cfw {

// The orientation in a JPEG's EXIF block (APP1), 1-8, or nothing when there
// is no EXIF block, no orientation tag, or the block is malformed. Stops at
// the first scan; never reads past `data`.
[[nodiscard]] std::optional<int> jpegExifOrientation(Span<const std::byte> data) noexcept;

// The image turned for display. Orientation 1, or any value outside 1-8,
// returns a copy.
[[nodiscard]] Result<Image> orientImage(const Image &image, int exifOrientation, const ImageLimits &limits = {});

} // namespace cfw
