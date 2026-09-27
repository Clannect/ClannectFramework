#pragma once

#include <array>
#include <cstdint>
#include <ostream>

namespace cfw {

struct LinearColor;

// A colour as creators see it: sRGB-encoded channels in [0, 1] with straight
// (not premultiplied) alpha. This is what the Properties panel edits and what
// scene files store (as 8-bit RGBA).
//
// Colour maths (blending, lighting, interpolation) belongs in linear space:
// convert with toLinear() and back with LinearColor::toSrgb(). The two types
// exist so a shader never receives sRGB values by accident.
//
// Threads: a plain value type. Allocates: nothing.
struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    // From 8-bit channels. fromRgba8(c.toRgba8()) round-trips every 8-bit
    // value exactly.
    [[nodiscard]] static constexpr Color fromRgba8(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                                   std::uint8_t a = 255) noexcept {
        return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }

    // Channels clamped to [0, 1] and rounded to the nearest 8-bit value.
    [[nodiscard]] std::array<std::uint8_t, 4> toRgba8() const noexcept;

    [[nodiscard]] LinearColor toLinear() const noexcept;

    friend constexpr bool operator==(Color x, Color y) noexcept = default;

    friend std::ostream &operator<<(std::ostream &out, Color c) {
        return out << "Color(" << c.r << ", " << c.g << ", " << c.b << ", " << c.a << ')';
    }
};

// Linear-light channels, straight alpha. Not clamped: HDR values above 1 are
// allowed.
struct LinearColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    // sRGB encoding (IEC 61966-2-1). Alpha is copied unchanged.
    [[nodiscard]] Color toSrgb() const noexcept;

    friend constexpr bool operator==(LinearColor x, LinearColor y) noexcept = default;

    friend std::ostream &operator<<(std::ostream &out, LinearColor c) {
        return out << "LinearColor(" << c.r << ", " << c.g << ", " << c.b << ", " << c.a << ')';
    }
};

// The sRGB transfer function and its inverse for one channel.
[[nodiscard]] float srgbToLinear(float encoded) noexcept;
[[nodiscard]] float linearToSrgb(float linear) noexcept;

} // namespace cfw
