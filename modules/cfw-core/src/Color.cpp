#include "cfw/core/Color.h"

#include <algorithm>
#include <cmath>

namespace cfw {

namespace {

std::uint8_t toByte(float channel) noexcept {
    // NaN clamps to 0: std::clamp would pass it through and the cast is UB.
    const float clamped = channel >= 0.0f ? std::min(channel, 1.0f) : 0.0f;
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
}

} // namespace

float srgbToLinear(float encoded) noexcept {
    return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float linear) noexcept {
    return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

std::array<std::uint8_t, 4> Color::toRgba8() const noexcept {
    return {toByte(r), toByte(g), toByte(b), toByte(a)};
}

LinearColor Color::toLinear() const noexcept {
    return {srgbToLinear(r), srgbToLinear(g), srgbToLinear(b), a};
}

Color LinearColor::toSrgb() const noexcept {
    return {linearToSrgb(r), linearToSrgb(g), linearToSrgb(b), a};
}

} // namespace cfw
