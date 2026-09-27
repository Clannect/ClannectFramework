#pragma once

#include <array>
#include <optional>
#include <ostream>

#include "cfw/core/Mat3.h"
#include "cfw/core/Quat.h"
#include "cfw/core/Rect.h"
#include "cfw/core/Vec3.h"
#include "cfw/core/Vec4.h"

namespace cfw {

// A 4x4 float matrix for 3D transforms and projections. Column vectors
// (v' = M * v), column-major storage (the layout OpenGL expects), and
// right-to-left composition: (A * B) applies B first. Projections are OpenGL
// style: right-handed view space looking down -Z, clip depth in [-1, 1].
//
// Build transforms from factories instead of mutating in place:
//     Mat4 model = Mat4::translation(pos) * Mat4::rotation(rot) * Mat4::scaling(size);
//
// Threads: a plain value type. Allocates: nothing.
class Mat4 {
public:
    // Identity.
    constexpr Mat4() noexcept : m_columns{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}} {}

    [[nodiscard]] static constexpr Mat4 fromColumns(Vec4 c0, Vec4 c1, Vec4 c2, Vec4 c3) noexcept {
        Mat4 m;
        m.m_columns = {c0, c1, c2, c3};
        return m;
    }
    // Rows as you would write them on paper.
    [[nodiscard]] static constexpr Mat4 fromRows(Vec4 r0, Vec4 r1, Vec4 r2, Vec4 r3) noexcept {
        return fromColumns({r0.x, r1.x, r2.x, r3.x}, {r0.y, r1.y, r2.y, r3.y}, {r0.z, r1.z, r2.z, r3.z},
                           {r0.w, r1.w, r2.w, r3.w});
    }

    [[nodiscard]] static constexpr Mat4 translation(Vec3 offset) noexcept {
        Mat4 m;
        m.m_columns[3] = {offset.x, offset.y, offset.z, 1.0f};
        return m;
    }
    [[nodiscard]] static constexpr Mat4 scaling(Vec3 factors) noexcept {
        return fromColumns({factors.x, 0, 0, 0}, {0, factors.y, 0, 0}, {0, 0, factors.z, 0}, {0, 0, 0, 1});
    }
    [[nodiscard]] static Mat4 rotation(Quat q) noexcept;
    [[nodiscard]] static Mat4 rotation(Vec3 axis, float degrees) noexcept;

    // Perspective projection. `verticalFovDegrees` is the full vertical angle.
    // Degenerate input (zero angle, near == far) gives the identity.
    [[nodiscard]] static Mat4 perspective(float verticalFovDegrees, float aspect, float nearPlane,
                                          float farPlane) noexcept;
    // Orthographic projection. Degenerate input gives the identity.
    [[nodiscard]] static Mat4 ortho(float left, float right, float bottom, float top, float nearPlane,
                                    float farPlane) noexcept;
    // View matrix looking from `eye` toward `center`. eye == center gives the identity.
    [[nodiscard]] static Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) noexcept;

    [[nodiscard]] constexpr float operator()(int row, int column) const noexcept {
        const Vec4 &c = m_columns[static_cast<std::size_t>(column)];
        return row == 0 ? c.x : row == 1 ? c.y : row == 2 ? c.z : c.w;
    }
    [[nodiscard]] constexpr Vec4 column(int index) const noexcept {
        return m_columns[static_cast<std::size_t>(index)];
    }
    // Pointer to 16 floats, column-major, for glUniformMatrix4fv.
    [[nodiscard]] const float *data() const noexcept { return &m_columns[0].x; }

    [[nodiscard]] Mat4 transposed() const noexcept;
    // Nothing if the matrix is singular.
    [[nodiscard]] std::optional<Mat4> inverse() const noexcept;
    // Inverse transpose of the upper-left 3x3, for transforming normals. A
    // singular matrix gives the identity.
    [[nodiscard]] Mat3 normalMatrix() const noexcept;
    [[nodiscard]] constexpr bool isAffine() const noexcept {
        return m_columns[0].w == 0.0f && m_columns[1].w == 0.0f && m_columns[2].w == 0.0f && m_columns[3].w == 1.0f;
    }

    // Transforms a point (w = 1), dividing by the resulting w.
    [[nodiscard]] Vec3 transformPoint(Vec3 p) const noexcept;
    // Transforms a direction (w = 0): no translation, no perspective divide.
    [[nodiscard]] constexpr Vec3 transformVector(Vec3 v) const noexcept {
        return (m_columns[0] * v.x + m_columns[1] * v.y + m_columns[2] * v.z).xyz();
    }
    // Bounding rectangle, in the XY plane, of the transformed rectangle (z = 0).
    [[nodiscard]] RectF transformRect(const RectF &rect) const noexcept;

    friend constexpr Vec4 operator*(const Mat4 &m, Vec4 v) noexcept {
        return m.m_columns[0] * v.x + m.m_columns[1] * v.y + m.m_columns[2] * v.z + m.m_columns[3] * v.w;
    }
    friend constexpr Mat4 operator*(const Mat4 &a, const Mat4 &b) noexcept {
        return fromColumns(a * b.m_columns[0], a * b.m_columns[1], a * b.m_columns[2], a * b.m_columns[3]);
    }
    friend constexpr bool operator==(const Mat4 &a, const Mat4 &b) noexcept = default;

    friend std::ostream &operator<<(std::ostream &out, const Mat4 &m) {
        out << "Mat4(";
        for (int r = 0; r < 4; ++r) {
            out << (r ? "; " : "") << m(r, 0) << ", " << m(r, 1) << ", " << m(r, 2) << ", " << m(r, 3);
        }
        return out << ')';
    }

private:
    std::array<Vec4, 4> m_columns;
};

// Every element within `tolerance`.
[[nodiscard]] bool nearlyEqual(const Mat4 &a, const Mat4 &b, float tolerance = 1e-5f) noexcept;

} // namespace cfw
