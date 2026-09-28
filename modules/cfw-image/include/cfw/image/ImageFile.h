#pragma once

// Decoding an image file whose format is not known in advance: the format
// is detected from the first bytes (never from a file name), then the
// matching decoder runs.
//
// Threads: any (pure functions). Allocates: see each decoder.

#include <cstddef>
#include <cstdint>
#include <optional>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"
#include "cfw/image/Image.h"

namespace cfw {

enum class ImageFormat : std::uint8_t { Png, Jpeg, WebP };

// Whether decodeImage() turns a photo the way its EXIF orientation says
// (JPEG only), as QImageReader::setAutoTransform(true) did.
enum class ImageOrientation : std::uint8_t { AsStored, ApplyExif };

// "PNG", "JPEG" or "WebP".
[[nodiscard]] StringView imageFormatName(ImageFormat format) noexcept;

// The format `data` starts with, or nothing if it is not one CFW decodes.
[[nodiscard]] std::optional<ImageFormat> detectImageFormat(Span<const std::byte> data) noexcept;

// Detects the format and decodes. Fails with Unsupported for an
// unrecognised format, otherwise as the matching decoder fails.
[[nodiscard]] Result<Image> decodeImage(Span<const std::byte> data, const ImageLimits &limits = {});
[[nodiscard]] Result<Image> decodeImage(Span<const std::byte> data, const ImageLimits &limits,
                                        ImageOrientation orientation);

} // namespace cfw
