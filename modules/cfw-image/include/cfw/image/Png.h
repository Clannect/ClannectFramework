#pragma once

// PNG (ISO/IEC 15948, W3C PNG 2nd edition), written for CFW on top of
// cfw-core's DEFLATE: no libpng.
//
// Decoding accepts every standard PNG: all colour types and bit depths
// (1, 2, 4, 8, 16), palettes, tRNS transparency and Adam7 interlacing. The
// result is always 8-bit straight RGBA; 16-bit samples keep their high byte.
// Colour-management chunks (gAMA, cHRM, iCCP, sRGB) are ignored, and APNG
// files decode to their default image.
//
// Hostile input fails cleanly: the size is checked against ImageLimits
// before anything is allocated, every chunk CRC is verified (a damaged
// ancillary chunk is skipped, a damaged critical chunk is an error), the
// compressed data may not expand past the exact size the header implies, and
// unknown critical chunks are rejected.
//
// Threads: any (pure functions). Allocates: the image plus one buffer the
// size of the filtered image data.

#include <cstddef>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/image/Image.h"

namespace cfw {

struct PngEncodeOptions {
    // DEFLATE level, 0-9 (see CompressOptions).
    int compressionLevel = 6;
};

[[nodiscard]] Result<Image> decodePng(Span<const std::byte> data, const ImageLimits &limits = {});

// Writes an 8-bit RGBA PNG, or RGB when the image is fully opaque. A
// premultiplied image is converted back to straight alpha on the way.
// Per-row filters are chosen adaptively (minimum sum of absolute
// differences, the heuristic libpng uses).
[[nodiscard]] Result<std::vector<std::byte>> encodePng(const Image &image, const PngEncodeOptions &options = {});

// True if `data` starts with the PNG signature.
[[nodiscard]] bool isPng(Span<const std::byte> data) noexcept;

} // namespace cfw
