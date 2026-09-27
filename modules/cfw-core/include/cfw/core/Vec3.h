#pragma once

#include <cmath>
#include <ostream>

namespace cfw {

// A 3D float vector: world positions, directions, sizes, Euler angles.
// Threads: a plain value type. Allocates: nothing.
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    friend constexpr bool operator==(Vec3 a, Vec3 b) noexcept = default;

    constexpr Vec3 &operator+=(Vec3 o) noexcept { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr Vec3 &operator-=(Vec3 o) noexcept { x -= o.x; y -= o.y; z -= o.z; return *this; }
    constexpr Vec3 &operator*=(float s) noexcept { x *= s; y *= s; z *= s; return *this; }
    constexpr Vec3 &operator/=(float s) noexcept { x /= s; y /= s; z /= s; return *this; }

    friend constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept { return a += b; }
    friend constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept { return a -= b; }
    friend constexpr Vec3 operator*(Vec3 a, float s) noexcept { return a *= s; }
    friend constexpr Vec3 operator*(float s, Vec3 a) noexcept { return a *= s; }
    friend constexpr Vec3 operator/(Vec3 a, float s) noexcept { return a /= s; }
    friend constexpr Vec3 operator-(Vec3 a) noexcept { return {-a.x, -a.y, -a.z}; }

    [[nodiscard]] constexpr Vec3 scaled(Vec3 o) const noexcept { return {x * o.x, y * o.y, z * o.z}; }
    [[nodiscard]] constexpr float dot(Vec3 o) const noexcept { return x * o.x + y * o.y + z * o.z; }
    [[nodiscard]] constexpr Vec3 cross(Vec3 o) const noexcept {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    [[nodiscard]] constexpr float lengthSquared() const noexcept { return dot(*this); }
    [[nodiscard]] float length() const noexcept { return std::sqrt(lengthSquared()); }
    // The unit vector, or zero for a zero-length vector.
    [[nodiscard]] Vec3 normalized() const noexcept {
        const float len = length();
        return len > 0.0f ? *this / len : Vec3{};
    }

    friend std::ostream &operator<<(std::ostream &out, Vec3 v) {
        return out << '(' << v.x << ", " << v.y << ", " << v.z << ')';
    }
};

[[nodiscard]] inline bool nearlyEqual(Vec3 a, Vec3 b, float tolerance = 1e-5f) noexcept {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance && std::abs(a.z - b.z) <= tolerance;
}

} // namespace cfw
