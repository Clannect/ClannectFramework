#pragma once

// JPEG (ITU-T T.81, JFIF, Adobe APP14), written for CFW: no libjpeg.
//
// Decoding covers what cameras, browsers and image editors produce:
// baseline and extended sequential and progressive Huffman-coded 8-bit
// JPEGs, any integer chroma subsampling, restart markers, and grayscale,
// YCbCr, RGB, CMYK and YCCK colour. Arithmetic coding, 12-bit precision,
// lossless and hierarchical JPEG are Unsupported (none of Clannect's sources
// produce them).
//
// The output matches libjpeg-turbo's default decode exactly: the same
// integer IDCT, "fancy" (triangle-filter) chroma upsampling and fixed-point
// colour conversion, so pixels are identical to what libjpeg-based tools
// show. CMYK is converted the way Qt did (Adobe's inverted CMYK, each channel
// multiplied by K). EXIF orientation is not applied.
//
// Hostile input fails cleanly: the size is checked against ImageLimits
// before allocating, table and scan parameters are validated, progressive
// files are limited to kMaxJpegScans scans, and truncation is an error:
// entropy data that runs out, or a progressive file without its end marker
// (it would otherwise decode to a blurrier image). A baseline file whose
// data is all present decodes even without the end marker, as in libjpeg.
//
// Threads: any (pure functions). Allocates: the image, plus two bytes per
// coefficient and one byte per sample while decoding.

#include <cstddef>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/image/Image.h"

namespace cfw {

// Progressive files with more scans than this are rejected: a few hundred
// bytes of empty scans could otherwise cost seconds each on a large image.
inline constexpr int kMaxJpegScans = 256;

struct JpegEncodeOptions {
    // 1-100, the IJG quality scale (75 is libjpeg's and Qt's default).
    int quality = 75;
    // 4:2:0 chroma subsampling (smaller files); false writes 4:4:4.
    bool subsampleChroma = true;
};

[[nodiscard]] Result<Image> decodeJpeg(Span<const std::byte> data, const ImageLimits &limits = {});

// Writes a baseline JFIF JPEG with the standard Huffman tables. Alpha is
// dropped (JPEG has none); a premultiplied image is converted back to
// straight alpha first.
[[nodiscard]] Result<std::vector<std::byte>> encodeJpeg(const Image &image, const JpegEncodeOptions &options = {});

// True if `data` starts with a JPEG start-of-image marker.
[[nodiscard]] bool isJpeg(Span<const std::byte> data) noexcept;

} // namespace cfw
