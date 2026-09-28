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

Hsv Color::toHsv() const noexcept {
    const float maximum = std::max({r, g, b});
    const float minimum = std::min({r, g, b});
    const float delta = maximum - minimum;
    Hsv hsv;
    hsv.v = maximum;
    hsv.a = a;
    if (maximum <= 0.0f || delta <= 0.0f) {
        return hsv; // black or grey: no hue, no saturation
    }
    hsv.s = delta / maximum;
    float hue = 0.0f;
    if (maximum == r) {
        hue = (g - b) / delta;
    } else if (maximum == g) {
        hue = 2.0f + (b - r) / delta;
    } else {
        hue = 4.0f + (r - g) / delta;
    }
    hue *= 60.0f;
    if (hue < 0.0f) {
        hue += 360.0f;
    }
    hsv.h = hue >= 360.0f ? 0.0f : hue;
    return hsv;
}

Color Color::fromHsv(const Hsv &hsv) noexcept {
    const float s = std::clamp(hsv.s, 0.0f, 1.0f);
    const float v = std::clamp(hsv.v, 0.0f, 1.0f);
    float h = std::fmod(hsv.h, 360.0f);
    if (h < 0.0f) {
        h += 360.0f;
    }
    const float sector = h / 60.0f;
    const int i = std::min(5, int(sector));
    const float f = sector - float(i);
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));
    switch (i) {
    case 0: return {v, t, p, hsv.a};
    case 1: return {q, v, p, hsv.a};
    case 2: return {p, v, t, hsv.a};
    case 3: return {p, q, v, hsv.a};
    case 4: return {t, p, v, hsv.a};
    default: return {v, p, q, hsv.a};
    }
}

std::string Color::toHex() const {
    static constexpr char kDigits[] = "0123456789abcdef";
    const std::array<std::uint8_t, 4> rgba = toRgba8();
    std::string out = "#";
    for (std::size_t i = 0; i < (rgba[3] == 255 ? 3u : 4u); ++i) {
        out += kDigits[rgba[i] >> 4];
        out += kDigits[rgba[i] & 15];
    }
    return out;
}

std::optional<Color> Color::fromHex(std::string_view text) noexcept {
    if (!text.empty() && text.front() == '#') {
        text.remove_prefix(1);
    }
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (const char c : text) {
        if (digit(c) < 0) {
            return std::nullopt;
        }
    }
    if (text.size() == 3) {
        const auto channel = [&](std::size_t i) { return std::uint8_t(digit(text[i]) * 17); };
        return fromRgba8(channel(0), channel(1), channel(2));
    }
    if (text.size() == 6 || text.size() == 8) {
        const auto channel = [&](std::size_t i) { return std::uint8_t(digit(text[2 * i]) * 16 + digit(text[2 * i + 1])); };
        return fromRgba8(channel(0), channel(1), channel(2), text.size() == 8 ? channel(3) : std::uint8_t(255));
    }
    return std::nullopt;
}

} // namespace cfw
