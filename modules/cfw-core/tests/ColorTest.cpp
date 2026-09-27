// Color: exact 8-bit round-trips (scene files store RGBA8), the sRGB transfer
// function, and clamping of out-of-range and NaN channels.

#include "cfw/core/Color.h"

#include <cmath>
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

int main() {
    everyByteRoundTrips();
    everyByteSurvivesLinearSpace();
    transferFunctionKnownValues();
    clampsBadChannels();
    alphaIsNotGammaEncoded();
    return cfw::test::finish("ColorTest");
}
