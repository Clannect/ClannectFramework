#include "cfw/image/ImageFile.h"

#include "cfw/image/Jpeg.h"
#include "cfw/image/Png.h"
#include "cfw/image/WebP.h"

namespace cfw {

StringView imageFormatName(ImageFormat format) noexcept {
    switch (format) {
    case ImageFormat::Png: return "PNG";
    case ImageFormat::Jpeg: return "JPEG";
    case ImageFormat::WebP: return "WebP";
    }
    return "?";
}

std::optional<ImageFormat> detectImageFormat(Span<const std::byte> data) noexcept {
    if (isPng(data)) {
        return ImageFormat::Png;
    }
    if (isJpeg(data)) {
        return ImageFormat::Jpeg;
    }
    if (isWebP(data)) {
        return ImageFormat::WebP;
    }
    return std::nullopt;
}

Result<Image> decodeImage(Span<const std::byte> data, const ImageLimits &limits) {
    const std::optional<ImageFormat> format = detectImageFormat(data);
    if (!format) {
        return Error(ErrorCode::Unsupported, "not a PNG, JPEG or WebP image");
    }
    switch (*format) {
    case ImageFormat::Png: return decodePng(data, limits);
    case ImageFormat::Jpeg: return decodeJpeg(data, limits);
    case ImageFormat::WebP: return decodeWebP(data, limits);
    }
    return Error(ErrorCode::Unsupported, "unknown image format");
}

} // namespace cfw
