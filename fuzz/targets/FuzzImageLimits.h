#pragma once

// Limits for the image fuzz targets: small, so hostile headers are rejected
// quickly and every accepted image is cheap to check.

#include <cstdlib>

#include "cfw/image/Image.h"

inline cfw::ImageLimits fuzzImageLimits() {
    cfw::ImageLimits limits;
    limits.maxWidth = 1024;
    limits.maxHeight = 1024;
    limits.maxDecodedBytes = 4u * 1024u * 1024u;
    return limits;
}

inline void checkDecoded(const cfw::Image &image, const cfw::ImageLimits &limits) {
    if (image.width() == 0 || image.height() == 0 || image.width() > limits.maxWidth ||
        image.height() > limits.maxHeight ||
        image.pixels().size() != std::size_t{image.width()} * image.height() * 4) {
        std::abort();
    }
}
