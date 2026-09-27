#pragma once

#include <cstdint>

#include "cfw/core/Result.h"
#include "cfw/image/Image.h"

namespace cfw {

enum class ResizeFilter : std::uint8_t {
    // Exact area average: each output pixel is the mean of the source area it
    // covers. The right choice for integer downscales (mipmaps, thumbnails).
    Box,
    // Triangle filter; plain bilinear interpolation when enlarging, widened
    // to cover the source footprint when shrinking so nothing aliases.
    Bilinear,
    // Lanczos with a = 3: the sharpest, with slight ringing at hard edges.
    Lanczos3,
};

// A resized copy. Filtering happens in premultiplied alpha, so colour never
// bleeds out of fully transparent pixels; the result keeps the source's
// alpha mode. Fails with InvalidArgument for an empty source or a zero
// target size, LimitExceeded if the target (or the working buffer) breaks
// `limits`, and OutOfMemory if an allocation fails.
//
// Threads: any. Allocates: the result and one float working buffer of
// min(width x source height, source width x height) pixels.
[[nodiscard]] Result<Image> resizeImage(const Image &source, std::uint32_t width, std::uint32_t height,
                                        ResizeFilter filter = ResizeFilter::Bilinear, const ImageLimits &limits = {});

} // namespace cfw
