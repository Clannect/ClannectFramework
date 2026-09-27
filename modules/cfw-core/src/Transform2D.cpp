#include "cfw/core/Transform2D.h"

#include <algorithm>
#include <cmath>

#include "cfw/core/MathUtil.h"

namespace cfw {

namespace {

// Points at or behind the projection's vanishing line are pushed to a tiny
// positive w, so they map far away but stay finite.
constexpr double kNearClip = 0.000001;

} // namespace

Transform2D Transform2D::rotation(double degrees) noexcept {
    // Exact results for quarter turns, so 90° rotations of pixel-aligned
    // content stay pixel-aligned.
    double s = 0.0;
    double c = 1.0;
    const double normalized = std::fmod(degrees, 360.0);
    if (normalized == 90.0 || normalized == -270.0) {
        s = 1.0;
        c = 0.0;
    } else if (normalized == 180.0 || normalized == -180.0) {
        s = 0.0;
        c = -1.0;
    } else if (normalized == 270.0 || normalized == -90.0) {
        s = -1.0;
        c = 0.0;
    } else if (normalized != 0.0) {
        const double radians = degreesToRadians(degrees);
        s = std::sin(radians);
        c = std::cos(radians);
    }
    return fromRows(c, -s, 0, s, c, 0);
}

std::optional<Transform2D> Transform2D::squareToQuad(const std::array<Vec2, 4> &quad) noexcept {
    const double dx0 = quad[0].x, dy0 = quad[0].y;
    const double dx1 = quad[1].x, dy1 = quad[1].y;
    const double dx2 = quad[2].x, dy2 = quad[2].y;
    const double dx3 = quad[3].x, dy3 = quad[3].y;

    const double ax = dx0 - dx1 + dx2 - dx3;
    const double ay = dy0 - dy1 + dy2 - dy3;
    if (ax == 0.0 && ay == 0.0) {
        // A parallelogram: the map is affine. A collapsed one (zero area)
        // would squash everything onto a line or point: refuse it.
        const Transform2D affine = fromRows(dx1 - dx0, dx2 - dx1, dx0, dy1 - dy0, dy2 - dy1, dy0);
        if (nearlyZero(affine.determinant())) {
            return std::nullopt;
        }
        return affine;
    }

    const double ax1 = dx1 - dx2, ax2 = dx3 - dx2;
    const double ay1 = dy1 - dy2, ay2 = dy3 - dy2;
    const double gtop = ax * ay2 - ax2 * ay;
    const double htop = ax1 * ay - ax * ay1;
    const double bottom = ax1 * ay2 - ax2 * ay1;
    if (bottom == 0.0) {
        return std::nullopt;
    }
    const double g = gtop / bottom;
    const double h = htop / bottom;
    const double a = dx1 - dx0 + g * dx1;
    const double b = dx3 - dx0 + h * dx3;
    const double d = dy1 - dy0 + g * dy1;
    const double e = dy3 - dy0 + h * dy3;
    return fromRows(a, b, dx0, d, e, dy0, g, h, 1.0);
}

std::optional<Transform2D> Transform2D::quadToQuad(const std::array<Vec2, 4> &from,
                                                   const std::array<Vec2, 4> &to) noexcept {
    const std::optional<Transform2D> fromSquare = squareToQuad(from);
    const std::optional<Transform2D> toSquare = fromSquare ? fromSquare->inverse() : std::nullopt;
    const std::optional<Transform2D> toQuad = squareToQuad(to);
    if (!toSquare || !toQuad) {
        return std::nullopt;
    }
    return *toQuad * *toSquare;
}

double Transform2D::maxStretch(const RectF &region) const noexcept {
    double s = 0.0;
    if (isAffine()) {
        s = std::sqrt(std::max(m[0] * m[0] + m[3] * m[3], m[1] * m[1] + m[4] * m[4]));
    } else {
        const float e = std::max(std::abs(region.width), std::abs(region.height)) * 1e-3f + 1e-3f;
        for (const Vec2 p : {Vec2{region.x, region.y}, Vec2{region.right(), region.y}, Vec2{region.x, region.bottom()},
                             Vec2{region.right(), region.bottom()}}) {
            const Vec2 o = map(p);
            const Vec2 dx = map({p.x + e, p.y});
            const Vec2 dy = map({p.x, p.y + e});
            s = std::max({s, static_cast<double>(std::hypot(dx.x - o.x, dx.y - o.y)) / e,
                          static_cast<double>(std::hypot(dy.x - o.x, dy.y - o.y)) / e});
        }
    }
    return s > 1e-9 && std::isfinite(s) ? s : 1.0;
}

std::optional<Transform2D> Transform2D::inverse() const noexcept {
    const double det = determinant();
    if (nearlyZero(det)) {
        return std::nullopt;
    }
    const auto e = [this](int r, int c) { return (*this)(r, c); };
    // Adjugate (transposed cofactors) divided by the determinant.
    const auto cofactor = [&e](int r0, int r1, int c0, int c1) { return e(r0, c0) * e(r1, c1) - e(r0, c1) * e(r1, c0); };
    return fromRows(cofactor(1, 2, 1, 2) / det, -cofactor(0, 2, 1, 2) / det, cofactor(0, 1, 1, 2) / det,
                    -cofactor(1, 2, 0, 2) / det, cofactor(0, 2, 0, 2) / det, -cofactor(0, 1, 0, 2) / det,
                    cofactor(1, 2, 0, 1) / det, -cofactor(0, 2, 0, 1) / det, cofactor(0, 1, 0, 1) / det);
}

Vec2 Transform2D::map(Vec2 p) const noexcept {
    const double x = m[0] * p.x + m[1] * p.y + m[2];
    const double y = m[3] * p.x + m[4] * p.y + m[5];
    if (isAffine()) {
        return {static_cast<float>(x), static_cast<float>(y)};
    }
    double w = m[6] * p.x + m[7] * p.y + m[8];
    w = std::max(w, kNearClip);
    return {static_cast<float>(x / w), static_cast<float>(y / w)};
}

RectF Transform2D::mapRect(const RectF &rect) const noexcept {
    const Vec2 corners[4] = {map({rect.left(), rect.top()}), map({rect.right(), rect.top()}),
                             map({rect.right(), rect.bottom()}), map({rect.left(), rect.bottom()})};
    Vec2 lo = corners[0];
    Vec2 hi = corners[0];
    for (const Vec2 &c : corners) {
        lo = {std::min(lo.x, c.x), std::min(lo.y, c.y)};
        hi = {std::max(hi.x, c.x), std::max(hi.y, c.y)};
    }
    return RectF::fromCorners(lo, hi);
}

} // namespace cfw
