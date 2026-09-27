#include "cfw/core/Mat3.h"

#include "cfw/core/MathUtil.h"

namespace cfw {

std::optional<Mat3> Mat3::inverse() const noexcept {
    const float det = determinant();
    if (nearlyZero(det)) {
        return std::nullopt;
    }
    // The rows of the inverse are the cross products of column pairs / det.
    const Vec3 r0 = m_columns[1].cross(m_columns[2]) / det;
    const Vec3 r1 = m_columns[2].cross(m_columns[0]) / det;
    const Vec3 r2 = m_columns[0].cross(m_columns[1]) / det;
    return fromColumns(r0, r1, r2).transposed();
}

} // namespace cfw
