#pragma once

// RGBA (straight or premultiplied) to the 32-bit BGRX both window systems
// take, composited over black (internal).

#include <cstdint>
#include <vector>

#include "cfw/image/Image.h"

namespace cfw::detail {

inline void toBgrx(const Image &image, std::vector<std::uint32_t> &out) {
    out.resize(std::size_t(image.width()) * image.height());
    const bool premultiplied = image.alphaMode() == AlphaMode::Premultiplied;
    const std::uint8_t *p = image.pixels().data();
    for (std::size_t i = 0; i < out.size(); ++i, p += 4) {
        std::uint32_t r = p[0];
        std::uint32_t g = p[1];
        std::uint32_t b = p[2];
        if (!premultiplied && p[3] != 255) {
            r = (r * p[3] + 127) / 255;
            g = (g * p[3] + 127) / 255;
            b = (b * p[3] + 127) / 255;
        }
        out[i] = 0xFF000000u | r << 16 | g << 8 | b;
    }
}

} // namespace cfw::detail
