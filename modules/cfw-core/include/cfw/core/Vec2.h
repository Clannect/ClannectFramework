#pragma once

#include <cmath>
#include <ostream>

namespace cfw {

// A 2D float vector: UI positions and sizes, texture coordinates.
// Threads: a plain value type. Allocates: nothing.
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    friend constexpr bool operator==(Vec2 a, Vec2 b) noexcept = default;

    constexpr Vec2 &operator+=(Vec2 o) noexcept { x += o.x; y += o.y; return *this; }
    constexpr Vec2 &operator-=(Vec2 o) noexcept { x -= o.x; y -= o.y; return *this; }
    constexpr Vec2 &operator*=(float s) noexcept { x *= s; y *= s; return *this; }
    constexpr Vec2 &operator/=(float s) noexcept { x /= s; y /= s; return *this; }

    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept { return a += b; }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept { return a -= b; }
    friend constexpr Vec2 operator*(Vec2 a, float s) noexcept { return a *= s; }
    friend constexpr Vec2 operator*(float s, Vec2 a) noexcept { return a *= s; }
    friend constexpr Vec2 operator/(Vec2 a, float s) noexcept { return a /= s; }
    friend constexpr Vec2 operator-(Vec2 a) noexcept { return {-a.x, -a.y}; }

    // Component-wise product (scale-by-size in UI layout).
    [[nodiscard]] constexpr Vec2 scaled(Vec2 o) const noexcept { return {x * o.x, y * o.y}; }
    [[nodiscard]] constexpr float dot(Vec2 o) const noexcept { return x * o.x + y * o.y; }
    [[nodiscard]] constexpr float lengthSquared() const noexcept { return dot(*this); }
    [[nodiscard]] float length() const noexcept { return std::sqrt(lengthSquared()); }
    // The unit vector, or zero for a zero-length vector.
    [[nodiscard]] Vec2 normalized() const noexcept {
        const float len = length();
        return len > 0.0f ? *this / len : Vec2{};
    }

    friend std::ostream &operator<<(std::ostream &out, Vec2 v) { return out << '(' << v.x << ", " << v.y << ')'; }
};

// A 2D integer vector: pixel sizes, window positions, grid cells.
struct Vec2i {
    int x = 0;
    int y = 0;

    friend constexpr bool operator==(Vec2i a, Vec2i b) noexcept = default;
    friend constexpr Vec2i operator+(Vec2i a, Vec2i b) noexcept { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Vec2i operator-(Vec2i a, Vec2i b) noexcept { return {a.x - b.x, a.y - b.y}; }
    [[nodiscard]] constexpr Vec2 toVec2() const noexcept { return {static_cast<float>(x), static_cast<float>(y)}; }

    friend std::ostream &operator<<(std::ostream &out, Vec2i v) { return out << '(' << v.x << ", " << v.y << ')'; }
};

// |a - b| <= tolerance on every component.
[[nodiscard]] inline bool nearlyEqual(Vec2 a, Vec2 b, float tolerance = 1e-5f) noexcept {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance;
}

} // namespace cfw
