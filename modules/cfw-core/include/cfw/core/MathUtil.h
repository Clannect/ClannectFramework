#pragma once

// Scalar helpers shared by the math types.
// Threads: any (pure functions). Allocates: nothing.

#include <cmath>
#include <numbers>

namespace cfw {

inline constexpr float kPi = std::numbers::pi_v<float>;

[[nodiscard]] constexpr float degreesToRadians(float degrees) noexcept { return degrees * (kPi / 180.0f); }
[[nodiscard]] constexpr float radiansToDegrees(float radians) noexcept { return radians * (180.0f / kPi); }
[[nodiscard]] constexpr double degreesToRadians(double degrees) noexcept {
    return degrees * (std::numbers::pi / 180.0);
}

// "Close enough to zero" for geometry decisions (degenerate axis, zero-length
// direction). The threshold matches the one the engine's scenes were authored
// against, so edge cases (gimbal lock, collinear up vectors) resolve the same way.
[[nodiscard]] constexpr bool nearlyZero(float value) noexcept { return value <= 1e-5f && value >= -1e-5f; }
[[nodiscard]] constexpr bool nearlyZero(double value) noexcept { return value <= 1e-12 && value >= -1e-12; }

} // namespace cfw
