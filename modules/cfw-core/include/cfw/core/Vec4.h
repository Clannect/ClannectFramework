#pragma once

#include <cmath>
#include <ostream>

#include "cfw/core/Vec3.h"

namespace cfw {

// A 4D float vector: homogeneous coordinates, shader parameters.
// Threads: a plain value type. Allocates: nothing.
struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    friend constexpr bool operator==(Vec4 a, Vec4 b) noexcept = default;

    constexpr Vec4 &operator+=(Vec4 o) noexcept { x += o.x; y += o.y; z += o.z; w += o.w; return *this; }
    constexpr Vec4 &operator-=(Vec4 o) noexcept { x -= o.x; y -= o.y; z -= o.z; w -= o.w; return *this; }
    constexpr Vec4 &operator*=(float s) noexcept { x *= s; y *= s; z *= s; w *= s; return *this; }

    friend constexpr Vec4 operator+(Vec4 a, Vec4 b) noexcept { return a += b; }
    friend constexpr Vec4 operator-(Vec4 a, Vec4 b) noexcept { return a -= b; }
    friend constexpr Vec4 operator*(Vec4 a, float s) noexcept { return a *= s; }
    friend constexpr Vec4 operator*(float s, Vec4 a) noexcept { return a *= s; }

    [[nodiscard]] constexpr float dot(Vec4 o) const noexcept { return x * o.x + y * o.y + z * o.z + w * o.w; }
    [[nodiscard]] constexpr Vec3 xyz() const noexcept { return {x, y, z}; }

    friend std::ostream &operator<<(std::ostream &out, Vec4 v) {
        return out << '(' << v.x << ", " << v.y << ", " << v.z << ", " << v.w << ')';
    }
};

} // namespace cfw
