#pragma once

// WebP decoding (RIFF container, lossy VP8, lossless VP8L, ALPH alpha),
// written for CFW: no libwebp. Decode only; Clannect writes PNG and JPEG.
//
// Lossy images use libwebp's default "fancy" chroma upsampling and its
// fixed-point colour conversion, so the pixels match what dwebp and every
// libwebp-based viewer show. Lossless images are exact by definition.
// Animated files decode to their first frame, placed on a transparent
// canvas of the animation's size.
//
// Hostile input fails cleanly: sizes are checked against ImageLimits before
// allocating, chunk sizes are bounds-checked, prefix codes must be complete,
// back-references must stay inside the image, and a truncated partition or
// image stream is an error.
//
// Threads: any (pure functions). Allocates: the image plus working planes
// (lossy: 1.5 bytes per pixel; lossless: 4 bytes per pixel and the prefix
// codes).

#include <cstddef>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/image/Image.h"

namespace cfw {

[[nodiscard]] Result<Image> decodeWebP(Span<const std::byte> data, const ImageLimits &limits = {});

// True if `data` starts with a RIFF WEBP header.
[[nodiscard]] bool isWebP(Span<const std::byte> data) noexcept;

} // namespace cfw
