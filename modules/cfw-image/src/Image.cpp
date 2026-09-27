#include "cfw/image/Image.h"

#include <algorithm>
#include <new>
#include <string>

#include "cfw/core/Contract.h"

namespace cfw {

Result<void> checkImageSize(std::uint64_t width, std::uint64_t height, const ImageLimits &limits) {
    if (width == 0 || height == 0) {
        return Error(ErrorCode::InvalidArgument, "image has a zero dimension");
    }
    const auto sizeText = [&] { return std::to_string(width) + "x" + std::to_string(height); };
    if (width > limits.maxWidth || height > limits.maxHeight) {
        return Error(ErrorCode::LimitExceeded, "image dimensions exceed the limit").with("size", sizeText());
    }
    const std::uint64_t longer = std::max(width, height);
    const std::uint64_t shorter = std::min(width, height);
    if (longer / shorter > limits.maxAspectRatio) {
        return Error(ErrorCode::LimitExceeded, "image aspect ratio exceeds the limit").with("size", sizeText());
    }
    // Divide rather than multiply: two 32-bit dimensions can overflow 64 bits.
    if (width > limits.maxDecodedBytes / 4 / height) {
        return Error(ErrorCode::LimitExceeded, "decoded image would exceed the byte limit").with("size", sizeText());
    }
    return success();
}

Result<Image> Image::create(std::uint32_t width, std::uint32_t height, AlphaMode alpha, const ImageLimits &limits) {
    if (Result<void> ok = checkImageSize(width, height, limits); !ok) {
        return std::move(ok).error();
    }
    Image image;
    try {
        image.m_pixels.assign(std::size_t{width} * height * 4u, 0);
    } catch (const std::bad_alloc &) {
        return Error(ErrorCode::OutOfMemory, "out of memory allocating an image")
            .with("size", std::to_string(width) + "x" + std::to_string(height));
    }
    image.m_width = width;
    image.m_height = height;
    image.m_alpha = alpha;
    return image;
}

Result<Image> Image::copy() const {
    Image out;
    try {
        out.m_pixels = m_pixels;
    } catch (const std::bad_alloc &) {
        return Error(ErrorCode::OutOfMemory, "out of memory copying an image");
    }
    out.m_width = m_width;
    out.m_height = m_height;
    out.m_alpha = m_alpha;
    return out;
}

Span<std::uint8_t> Image::row(std::uint32_t y) noexcept {
    debugCheck(y < m_height, "Image::row out of range");
    return Span<std::uint8_t>(m_pixels).subspan(std::size_t{y} * stride(), stride());
}

Span<const std::uint8_t> Image::row(std::uint32_t y) const noexcept {
    debugCheck(y < m_height, "Image::row out of range");
    return Span<const std::uint8_t>(m_pixels).subspan(std::size_t{y} * stride(), stride());
}

void Image::premultiply() noexcept {
    if (m_alpha == AlphaMode::Premultiplied) {
        return;
    }
    for (std::size_t i = 0; i < m_pixels.size(); i += 4) {
        const unsigned a = m_pixels[i + 3];
        if (a == 255) {
            continue;
        }
        for (std::size_t c = 0; c < 3; ++c) {
            m_pixels[i + c] = static_cast<std::uint8_t>((m_pixels[i + c] * a + 127u) / 255u);
        }
    }
    m_alpha = AlphaMode::Premultiplied;
}

void Image::unpremultiply() noexcept {
    if (m_alpha == AlphaMode::Straight) {
        return;
    }
    for (std::size_t i = 0; i < m_pixels.size(); i += 4) {
        const unsigned a = m_pixels[i + 3];
        if (a == 255) {
            continue;
        }
        for (std::size_t c = 0; c < 3; ++c) {
            m_pixels[i + c] =
                a == 0 ? std::uint8_t{0} : static_cast<std::uint8_t>(std::min(255u, (m_pixels[i + c] * 255u + a / 2) / a));
        }
    }
    m_alpha = AlphaMode::Straight;
}

bool Image::isOpaque() const noexcept {
    for (std::size_t i = 3; i < m_pixels.size(); i += 4) {
        if (m_pixels[i] != 255) {
            return false;
        }
    }
    return true;
}

} // namespace cfw
