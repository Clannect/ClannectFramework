#pragma once

#include <algorithm>
#include <ostream>

#include "cfw/core/Vec2.h"

namespace cfw {

// An axis-aligned float rectangle: origin (top-left in UI space) and size.
// A rectangle with non-positive width or height is empty. Edges: `right()` is
// x + width, and contains() is half-open (left/top inclusive, right/bottom
// exclusive), so adjacent rectangles never both contain a point.
//
// Threads: a plain value type. Allocates: nothing.
struct RectF {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    [[nodiscard]] static constexpr RectF fromCorners(Vec2 a, Vec2 b) noexcept {
        const float left = std::min(a.x, b.x);
        const float top = std::min(a.y, b.y);
        return {left, top, std::max(a.x, b.x) - left, std::max(a.y, b.y) - top};
    }
    [[nodiscard]] static constexpr RectF fromOriginSize(Vec2 origin, Vec2 size) noexcept {
        return {origin.x, origin.y, size.x, size.y};
    }

    [[nodiscard]] constexpr float left() const noexcept { return x; }
    [[nodiscard]] constexpr float top() const noexcept { return y; }
    [[nodiscard]] constexpr float right() const noexcept { return x + width; }
    [[nodiscard]] constexpr float bottom() const noexcept { return y + height; }
    [[nodiscard]] constexpr Vec2 origin() const noexcept { return {x, y}; }
    [[nodiscard]] constexpr Vec2 size() const noexcept { return {width, height}; }
    [[nodiscard]] constexpr Vec2 center() const noexcept { return {x + width * 0.5f, y + height * 0.5f}; }
    [[nodiscard]] constexpr bool isEmpty() const noexcept { return !(width > 0.0f && height > 0.0f); }

    [[nodiscard]] constexpr bool contains(Vec2 p) const noexcept {
        return p.x >= x && p.y >= y && p.x < right() && p.y < bottom();
    }
    [[nodiscard]] constexpr bool contains(const RectF &r) const noexcept {
        return r.x >= x && r.y >= y && r.right() <= right() && r.bottom() <= bottom();
    }
    [[nodiscard]] constexpr bool intersects(const RectF &r) const noexcept {
        return !isEmpty() && !r.isEmpty() && r.x < right() && x < r.right() && r.y < bottom() && y < r.bottom();
    }
    // The overlap, or an empty rectangle at the origin if they do not overlap.
    [[nodiscard]] constexpr RectF intersected(const RectF &r) const noexcept {
        if (!intersects(r)) {
            return {};
        }
        const float left = std::max(x, r.x);
        const float top = std::max(y, r.y);
        return {left, top, std::min(right(), r.right()) - left, std::min(bottom(), r.bottom()) - top};
    }
    // The bounding rectangle of both. An empty rectangle contributes nothing.
    [[nodiscard]] constexpr RectF united(const RectF &r) const noexcept {
        if (isEmpty()) {
            return r;
        }
        if (r.isEmpty()) {
            return *this;
        }
        return fromCorners({std::min(x, r.x), std::min(y, r.y)}, {std::max(right(), r.right()), std::max(bottom(), r.bottom())});
    }
    // Moves each edge outward by the given amounts (negative moves inward).
    [[nodiscard]] constexpr RectF grownBy(float left, float top, float right, float bottom) const noexcept {
        return {x - left, y - top, width + left + right, height + top + bottom};
    }
    [[nodiscard]] constexpr RectF translated(Vec2 offset) const noexcept {
        return {x + offset.x, y + offset.y, width, height};
    }

    friend constexpr bool operator==(const RectF &a, const RectF &b) noexcept = default;

    friend std::ostream &operator<<(std::ostream &out, const RectF &r) {
        return out << "RectF(" << r.x << ", " << r.y << ", " << r.width << " x " << r.height << ')';
    }
};

// An integer rectangle: pixels, damage regions, atlas slots. Same edge rules
// as RectF.
struct Recti {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr int right() const noexcept { return x + width; }
    [[nodiscard]] constexpr int bottom() const noexcept { return y + height; }
    [[nodiscard]] constexpr bool isEmpty() const noexcept { return width <= 0 || height <= 0; }
    [[nodiscard]] constexpr bool contains(Vec2i p) const noexcept {
        return p.x >= x && p.y >= y && p.x < right() && p.y < bottom();
    }
    [[nodiscard]] constexpr bool intersects(const Recti &r) const noexcept {
        return !isEmpty() && !r.isEmpty() && r.x < right() && x < r.right() && r.y < bottom() && y < r.bottom();
    }
    [[nodiscard]] constexpr Recti intersected(const Recti &r) const noexcept {
        if (!intersects(r)) {
            return {};
        }
        const int left = std::max(x, r.x);
        const int top = std::max(y, r.y);
        return {left, top, std::min(right(), r.right()) - left, std::min(bottom(), r.bottom()) - top};
    }
    [[nodiscard]] constexpr Recti united(const Recti &r) const noexcept {
        if (isEmpty()) {
            return r;
        }
        if (r.isEmpty()) {
            return *this;
        }
        const int left = std::min(x, r.x);
        const int top = std::min(y, r.y);
        return {left, top, std::max(right(), r.right()) - left, std::max(bottom(), r.bottom()) - top};
    }
    [[nodiscard]] constexpr RectF toRectF() const noexcept {
        return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height)};
    }

    friend constexpr bool operator==(const Recti &a, const Recti &b) noexcept = default;

    friend std::ostream &operator<<(std::ostream &out, const Recti &r) {
        return out << "Recti(" << r.x << ", " << r.y << ", " << r.width << " x " << r.height << ')';
    }
};

} // namespace cfw
