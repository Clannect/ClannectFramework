#pragma once

// Pieces of the WebP decoder shared between its source files.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/image/ImageLimits.h"

namespace cfw::webp {

// A decoded VP8L image: width x height ARGB pixels (0xAARRGGBB).
struct ArgbImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint32_t> pixels;
};

// A whole VP8L bitstream: signature, size header, then the image.
[[nodiscard]] Result<ArgbImage> decodeLossless(Span<const std::byte> data, const ImageLimits &limits);

// A headerless VP8L image stream of a known size (an ALPH chunk's lossless
// alpha, which lives in the green channel).
[[nodiscard]] Result<ArgbImage> decodeLosslessStream(Span<const std::byte> data, std::uint32_t width,
                                                     std::uint32_t height, const ImageLimits &limits);

// A VP8 key frame decoded to RGBA (alpha 255), using libwebp's default
// "fancy" chroma upsampling so the pixels match dwebp's.
struct Rgba {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};
[[nodiscard]] Result<Rgba> decodeLossy(Span<const std::byte> data, const ImageLimits &limits);

} // namespace cfw::webp
