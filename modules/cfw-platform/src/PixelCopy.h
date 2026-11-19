#pragma once

// RGBA (straight or premultiplied) to the 32-bit BGRX both window systems
// take, composited over black (internal).

#include <algorithm>
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

// Converts only the pixels in a rectangle; `out` keeps the image's size.
inline void toBgrx(const Image &image, std::vector<std::uint32_t> &out, int x0, int y0, int w, int h) {
    const int iw = int(image.width()), ih = int(image.height());
    if (out.size() != std::size_t(iw) * std::size_t(ih)) {
        toBgrx(image, out);
        return;
    }
    const int x1 = std::min(iw, x0 + w), y1 = std::min(ih, y0 + h);
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    const bool premultiplied = image.alphaMode() == AlphaMode::Premultiplied;
    for (int y = y0; y < y1; ++y) {
        const std::uint8_t *p = image.pixels().data() + (std::size_t(y) * std::size_t(iw) + std::size_t(x0)) * 4u;
        std::uint32_t *o = out.data() + std::size_t(y) * std::size_t(iw) + std::size_t(x0);
        for (int x = x0; x < x1; ++x, p += 4, ++o) {
            std::uint32_t r = p[0];
            std::uint32_t g = p[1];
            std::uint32_t b = p[2];
            if (!premultiplied && p[3] != 255) {
                r = (r * p[3] + 127) / 255;
                g = (g * p[3] + 127) / 255;
                b = (b * p[3] + 127) / 255;
            }
            *o = 0xFF000000u | r << 16 | g << 8 | b;
        }
    }
}

} // namespace cfw::detail
