#pragma once

#include <array>
#include <optional>
#include <ostream>

#include "cfw/core/Vec3.h"

namespace cfw {

// A 3x3 float matrix for 3D linear maps: rotations, normal matrices. Column
// vectors (v' = M * v), column-major storage, the same layout as a GLSL mat3.
//
// Threads: a plain value type. Allocates: nothing.
class Mat3 {
public:
    // Identity.
    constexpr Mat3() noexcept : m_columns{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}} {}

    [[nodiscard]] static constexpr Mat3 fromColumns(Vec3 c0, Vec3 c1, Vec3 c2) noexcept {
        Mat3 m;
        m.m_columns = {c0, c1, c2};
        return m;
    }

    [[nodiscard]] constexpr float operator()(int row, int column) const noexcept {
        const Vec3 &c = m_columns[static_cast<std::size_t>(column)];
        return row == 0 ? c.x : row == 1 ? c.y : c.z;
    }
    [[nodiscard]] constexpr Vec3 column(int index) const noexcept {
        return m_columns[static_cast<std::size_t>(index)];
    }
    // Pointer to 9 floats, column-major, for glUniformMatrix3fv.
    [[nodiscard]] const float *data() const noexcept { return &m_columns[0].x; }

    [[nodiscard]] constexpr Mat3 transposed() const noexcept {
        return fromColumns({m_columns[0].x, m_columns[1].x, m_columns[2].x},
                           {m_columns[0].y, m_columns[1].y, m_columns[2].y},
                           {m_columns[0].z, m_columns[1].z, m_columns[2].z});
    }
    [[nodiscard]] constexpr float determinant() const noexcept {
        return m_columns[0].dot(m_columns[1].cross(m_columns[2]));
    }
    // Nothing if the matrix is singular.
    [[nodiscard]] std::optional<Mat3> inverse() const noexcept;

    friend constexpr Vec3 operator*(const Mat3 &m, Vec3 v) noexcept {
        return m.m_columns[0] * v.x + m.m_columns[1] * v.y + m.m_columns[2] * v.z;
    }
    friend constexpr Mat3 operator*(const Mat3 &a, const Mat3 &b) noexcept {
        return fromColumns(a * b.m_columns[0], a * b.m_columns[1], a * b.m_columns[2]);
    }
    friend constexpr bool operator==(const Mat3 &a, const Mat3 &b) noexcept = default;

    friend std::ostream &operator<<(std::ostream &out, const Mat3 &m) {
        out << "Mat3(";
        for (int r = 0; r < 3; ++r) {
            out << (r ? "; " : "") << m(r, 0) << ", " << m(r, 1) << ", " << m(r, 2);
        }
        return out << ')';
    }

private:
    std::array<Vec3, 3> m_columns;
};

} // namespace cfw
