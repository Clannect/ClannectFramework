#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/image/ImageLimits.h"

namespace cfw {

// How the alpha channel relates to the colour channels.
enum class AlphaMode : std::uint8_t {
    Straight,      // what files store: colour is independent of alpha
    Premultiplied, // what the renderer blends: colour already multiplied by alpha
};

// An 8-bit RGBA image in memory: rows top to bottom, 4 bytes per pixel in
// R, G, B, A order, rows packed with no padding. Every decoder produces this
// and every encoder takes it; one format keeps the renderer and the codecs
// simple.
//
// Threads: a value type. Allocates: width * height * 4 bytes. Copies are
// explicit (copy()) because an image can be hundreds of megabytes.
class Image {
public:
    Image() = default;
    Image(Image &&) noexcept = default;
    Image &operator=(Image &&) noexcept = default;
    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;

    // A transparent black image. Fails with LimitExceeded if it breaks
    // `limits`, InvalidArgument for a zero dimension, and OutOfMemory if the
    // allocation fails (it never throws).
    [[nodiscard]] static Result<Image> create(std::uint32_t width, std::uint32_t height,
                                              AlphaMode alpha = AlphaMode::Straight, const ImageLimits &limits = {});

    [[nodiscard]] Result<Image> copy() const;

    [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
    [[nodiscard]] bool empty() const noexcept { return m_width == 0 || m_height == 0; }
    [[nodiscard]] std::size_t stride() const noexcept { return std::size_t{m_width} * 4u; }
    [[nodiscard]] AlphaMode alphaMode() const noexcept { return m_alpha; }

    [[nodiscard]] Span<std::uint8_t> pixels() noexcept { return m_pixels; }
    [[nodiscard]] Span<const std::uint8_t> pixels() const noexcept { return m_pixels; }
    // Row y: width() * 4 bytes. y must be < height() (checked in debug builds).
    [[nodiscard]] Span<std::uint8_t> row(std::uint32_t y) noexcept;
    [[nodiscard]] Span<const std::uint8_t> row(std::uint32_t y) const noexcept;

    // In place. Premultiplying rounds (c * a + 127) / 255; un-premultiplying a
    // pixel with alpha 0 gives transparent black. Both are no-ops if the image
    // is already in that mode.
    void premultiply() noexcept;
    void unpremultiply() noexcept;

    // True if every alpha value is 255.
    [[nodiscard]] bool isOpaque() const noexcept;

private:
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    AlphaMode m_alpha = AlphaMode::Straight;
    std::vector<std::uint8_t> m_pixels;
};

// Checks a width and height against `limits` without allocating. Decoders
// call this as soon as they read a header.
[[nodiscard]] Result<void> checkImageSize(std::uint64_t width, std::uint64_t height, const ImageLimits &limits);

} // namespace cfw
