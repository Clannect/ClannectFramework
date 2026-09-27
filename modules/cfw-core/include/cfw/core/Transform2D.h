#pragma once

#include <array>
#include <optional>
#include <ostream>

#include "cfw/core/Rect.h"
#include "cfw/core/Vec2.h"

namespace cfw {

// A 2D projective transform (3x3, double precision): what cfw::Painter draws
// with. Usually affine (translate/rotate/scale/shear); perspective appears
// through quadToQuad, which maps an interface onto a projected surface
// (SurfaceGui, decals).
//
// Column vectors and right-to-left composition, like Mat4: (A * B) applies B
// first. `a.then(b)` reads left to right and means "a, then b" (b * a).
//
//     | xx  xy  tx |   x' = (xx*x + xy*y + tx) / w
//     | yx  yy  ty |   y' = (yx*x + yy*y + ty) / w
//     | px  py  pw |   w  =  px*x + py*y + pw
//
// Doubles because perspective mapping of large surfaces loses visible
// precision in float, and the CPU rasteriser (Blend2D) takes doubles anyway.
//
// Threads: a plain value type. Allocates: nothing.
class Transform2D {
public:
    // Identity.
    constexpr Transform2D() noexcept = default;

    // From the nine elements, row by row.
    [[nodiscard]] static constexpr Transform2D fromRows(double xx, double xy, double tx, double yx, double yy,
                                                        double ty, double px = 0.0, double py = 0.0,
                                                        double pw = 1.0) noexcept {
        Transform2D t;
        t.m = {xx, xy, tx, yx, yy, ty, px, py, pw};
        return t;
    }
    [[nodiscard]] static constexpr Transform2D translation(double dx, double dy) noexcept {
        return fromRows(1, 0, dx, 0, 1, dy);
    }
    [[nodiscard]] static constexpr Transform2D scaling(double sx, double sy) noexcept {
        return fromRows(sx, 0, 0, 0, sy, 0);
    }
    // Counter-clockwise in a y-up space; clockwise on screen (y down).
    [[nodiscard]] static Transform2D rotation(double degrees) noexcept;

    // The projective map taking the unit square (0,0) (1,0) (1,1) (0,1) to
    // `quad`, corner for corner. Nothing if the quad is degenerate (zero
    // area, or three corners on a line).
    [[nodiscard]] static std::optional<Transform2D> squareToQuad(const std::array<Vec2, 4> &quad) noexcept;
    // The projective map taking quad `from` to quad `to`, corner for corner.
    // Nothing if either quad is degenerate.
    [[nodiscard]] static std::optional<Transform2D> quadToQuad(const std::array<Vec2, 4> &from,
                                                               const std::array<Vec2, 4> &to) noexcept;

    // Element at (row, column).
    [[nodiscard]] constexpr double operator()(int row, int column) const noexcept {
        return m[static_cast<std::size_t>(row * 3 + column)];
    }

    [[nodiscard]] constexpr bool isAffine() const noexcept { return m[6] == 0.0 && m[7] == 0.0 && m[8] == 1.0; }
    [[nodiscard]] constexpr bool isIdentity() const noexcept { return *this == Transform2D(); }
    [[nodiscard]] constexpr double determinant() const noexcept {
        return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
               m[2] * (m[3] * m[7] - m[4] * m[6]);
    }
    // Nothing if singular.
    [[nodiscard]] std::optional<Transform2D> inverse() const noexcept;

    // "This, then `next`".
    [[nodiscard]] constexpr Transform2D then(const Transform2D &next) const noexcept { return next * *this; }

    // Maps a point, dividing by w. Points at or behind the vanishing line
    // (w <= 1e-6) are clamped to w = 1e-6: they map far away but stay finite.
    [[nodiscard]] Vec2 map(Vec2 p) const noexcept;
    // Bounding rectangle of the mapped corners.
    [[nodiscard]] RectF mapRect(const RectF &rect) const noexcept;

    friend constexpr Transform2D operator*(const Transform2D &a, const Transform2D &b) noexcept {
        Transform2D r;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                double sum = 0.0;
                for (int k = 0; k < 3; ++k) {
                    sum += a(row, k) * b(k, col);
                }
                r.m[static_cast<std::size_t>(row * 3 + col)] = sum;
            }
        }
        return r;
    }
    friend constexpr bool operator==(const Transform2D &a, const Transform2D &b) noexcept = default;

    friend std::ostream &operator<<(std::ostream &out, const Transform2D &t) {
        out << "Transform2D(";
        for (int r = 0; r < 3; ++r) {
            out << (r ? "; " : "") << t(r, 0) << ", " << t(r, 1) << ", " << t(r, 2);
        }
        return out << ')';
    }

private:
    std::array<double, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1}; // row-major
};

} // namespace cfw
