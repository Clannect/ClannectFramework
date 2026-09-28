// Color: exact 8-bit round-trips (scene files store RGBA8), the sRGB transfer
// function, and clamping of out-of-range and NaN channels.

#include "cfw/core/Color.h"

#include <array>
#include <cmath>
#include <string>
#include <limits>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

void everyByteRoundTrips() {
    int failures = 0;
    for (int v = 0; v < 256; ++v) {
        const auto byte = static_cast<std::uint8_t>(v);
        const auto back = Color::fromRgba8(byte, byte, byte, byte).toRgba8();
        failures += back[0] == byte && back[3] == byte ? 0 : 1;
    }
    checkEqual(failures, 0, "8-bit -> Color -> 8-bit is exact for all 256 values");
}

void everyByteSurvivesLinearSpace() {
    int failures = 0;
    for (int v = 0; v < 256; ++v) {
        const auto byte = static_cast<std::uint8_t>(v);
        const auto back = Color::fromRgba8(byte, byte, byte).toLinear().toSrgb().toRgba8();
        failures += back[0] == byte ? 0 : 1;
    }
    checkEqual(failures, 0, "sRGB -> linear -> sRGB is exact at 8-bit precision");
}

void transferFunctionKnownValues() {
    checkNear(srgbToLinear(0.0f), 0.0, 1e-7, "black");
    checkNear(srgbToLinear(1.0f), 1.0, 1e-6, "white");
    checkNear(srgbToLinear(0.5f), 0.214041, 1e-5, "mid-grey is ~21% linear");
    checkNear(linearToSrgb(0.214041f), 0.5, 1e-5, "and back");
}

void clampsBadChannels() {
    const Color wild{2.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), 0.5f};
    const auto bytes = wild.toRgba8();
    checkEqual(static_cast<int>(bytes[0]), 255, "above 1 clamps to 255");
    checkEqual(static_cast<int>(bytes[1]), 0, "below 0 clamps to 0");
    checkEqual(static_cast<int>(bytes[2]), 0, "NaN becomes 0, never UB");
    checkEqual(static_cast<int>(bytes[3]), 128, "0.5 rounds to 128");
}

void alphaIsNotGammaEncoded() {
    const Color c{0.5f, 0.5f, 0.5f, 0.5f};
    checkEqual(c.toLinear().a, 0.5f, "alpha is linear already");
}

} // namespace

void hsv() {
    const Hsv red = Color{1, 0, 0, 1}.toHsv();
    checkNear(red.h, 0.0f, 1e-5f, "red's hue");
    checkNear(red.s, 1.0f, 1e-5f, "red is saturated");
    checkNear(Color{0, 1, 0, 1}.toHsv().h, 120.0f, 1e-4f, "green's hue");
    checkNear(Color{0, 0, 1, 1}.toHsv().h, 240.0f, 1e-4f, "blue's hue");
    checkNear(Color{1, 0, 1, 1}.toHsv().h, 300.0f, 1e-4f, "magenta's hue");
    const Hsv grey = Color{0.5f, 0.5f, 0.5f, 0.25f}.toHsv();
    check(grey.h == 0.0f && grey.s == 0.0f && grey.v == 0.5f && grey.a == 0.25f, "a grey has no hue or saturation");

    // Every 8-bit colour on a coarse grid survives HSV and back.
    int failures = 0;
    for (int r = 0; r < 256; r += 15) {
        for (int g = 0; g < 256; g += 15) {
            for (int b = 0; b < 256; b += 15) {
                const Color c = Color::fromRgba8(std::uint8_t(r), std::uint8_t(g), std::uint8_t(b));
                if (Color::fromHsv(c.toHsv()).toRgba8() != c.toRgba8()) {
                    ++failures;
                }
            }
        }
    }
    checkEqual(failures, 0, "HSV round-trips 8-bit colours");
    checkEqual(Color::fromHsv({360.0f + 120.0f, 1, 1, 1}).toRgba8(), (std::array<std::uint8_t, 4>{0, 255, 0, 255}),
               "hues wrap");
}

void hex() {
    checkEqual(Color::fromRgba8(0xfe, 0x41, 0x33).toHex(), std::string("#fe4133"), "opaque: six digits");
    checkEqual(Color::fromRgba8(0, 0, 0, 0x80).toHex(), std::string("#00000080"), "with alpha: eight");
    check(Color::fromHex("#FE4133") == Color::fromRgba8(0xfe, 0x41, 0x33), "upper case, with #");
    check(Color::fromHex("fe4133") == Color::fromRgba8(0xfe, 0x41, 0x33), "without #");
    check(Color::fromHex("#f00") == Color::fromRgba8(255, 0, 0), "three digits");
    check(Color::fromHex("#00000080") == Color::fromRgba8(0, 0, 0, 0x80), "eight digits");
    check(!Color::fromHex("#12345").has_value(), "five digits are nothing");
    check(!Color::fromHex("#gg0000").has_value(), "not hex");
    check(!Color::fromHex("").has_value(), "empty");
}

int main() {
    hsv();
    hex();
    everyByteRoundTrips();
    everyByteSurvivesLinearSpace();
    transferFunctionKnownValues();
    clampsBadChannels();
    alphaIsNotGammaEncoded();
    return cfw::test::finish("ColorTest");
}
