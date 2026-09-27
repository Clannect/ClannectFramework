#include "cfw/core/Mat4.h"

#include <algorithm>
#include <cmath>

#include "cfw/core/MathUtil.h"

namespace cfw {

Mat4 Mat4::rotation(Quat q) noexcept {
    const Mat3 r = q.toRotationMatrix();
    const Vec3 c0 = r.column(0), c1 = r.column(1), c2 = r.column(2);
    return fromColumns({c0.x, c0.y, c0.z, 0}, {c1.x, c1.y, c1.z, 0}, {c2.x, c2.y, c2.z, 0}, {0, 0, 0, 1});
}

Mat4 Mat4::rotation(Vec3 axis, float degrees) noexcept { return rotation(Quat::fromAxisAngle(axis, degrees)); }

Mat4 Mat4::perspective(float verticalFovDegrees, float aspect, float nearPlane, float farPlane) noexcept {
    const float half = degreesToRadians(verticalFovDegrees / 2.0f);
    const float sine = std::sin(half);
    const float clip = farPlane - nearPlane;
    if (sine == 0.0f || clip == 0.0f || aspect == 0.0f) {
        return {};
    }
    const float cotan = std::cos(half) / sine;
    return fromRows({cotan / aspect, 0, 0, 0}, {0, cotan, 0, 0},
                    {0, 0, -(nearPlane + farPlane) / clip, -(2.0f * nearPlane * farPlane) / clip}, {0, 0, -1, 0});
}

Mat4 Mat4::ortho(float left, float right, float bottom, float top, float nearPlane, float farPlane) noexcept {
    if (left == right || bottom == top || nearPlane == farPlane) {
        return {};
    }
    const float width = right - left;
    const float height = top - bottom;
    const float clip = farPlane - nearPlane;
    return fromRows({2.0f / width, 0, 0, -(left + right) / width}, {0, 2.0f / height, 0, -(top + bottom) / height},
                    {0, 0, -2.0f / clip, -(nearPlane + farPlane) / clip}, {0, 0, 0, 1});
}

Mat4 Mat4::lookAt(Vec3 eye, Vec3 center, Vec3 up) noexcept {
    Vec3 forward = center - eye;
    if (nearlyZero(forward.x) && nearlyZero(forward.y) && nearlyZero(forward.z)) {
        return {};
    }
    forward = forward.normalized();
    const Vec3 side = forward.cross(up).normalized();
    const Vec3 upVector = side.cross(forward);
    const Mat4 basis = fromRows({side.x, side.y, side.z, 0}, {upVector.x, upVector.y, upVector.z, 0},
                                {-forward.x, -forward.y, -forward.z, 0}, {0, 0, 0, 1});
    return basis * translation(-eye);
}

Mat4 Mat4::transposed() const noexcept {
    return fromRows(m_columns[0], m_columns[1], m_columns[2], m_columns[3]);
}

std::optional<Mat4> Mat4::inverse() const noexcept {
    // Cofactor expansion (the classic MESA formulation). It works on either
    // storage order, since inverse(transpose(M)) == transpose(inverse(M)).
    const float *m = data();
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (nearlyZero(det)) {
        return std::nullopt;
    }
    const float scale = 1.0f / det;
    return fromColumns({inv[0] * scale, inv[1] * scale, inv[2] * scale, inv[3] * scale},
                       {inv[4] * scale, inv[5] * scale, inv[6] * scale, inv[7] * scale},
                       {inv[8] * scale, inv[9] * scale, inv[10] * scale, inv[11] * scale},
                       {inv[12] * scale, inv[13] * scale, inv[14] * scale, inv[15] * scale});
}

Mat3 Mat4::normalMatrix() const noexcept {
    const Mat3 upper = Mat3::fromColumns(m_columns[0].xyz(), m_columns[1].xyz(), m_columns[2].xyz());
    const std::optional<Mat3> inverse = upper.inverse();
    return inverse ? inverse->transposed() : Mat3();
}

Vec3 Mat4::transformPoint(Vec3 p) const noexcept {
    const Vec4 r = *this * Vec4{p.x, p.y, p.z, 1.0f};
    if (r.w == 1.0f || r.w == 0.0f) {
        return r.xyz();
    }
    return r.xyz() / r.w;
}

RectF Mat4::transformRect(const RectF &rect) const noexcept {
    const Vec3 corners[4] = {transformPoint({rect.left(), rect.top(), 0}), transformPoint({rect.right(), rect.top(), 0}),
                             transformPoint({rect.right(), rect.bottom(), 0}),
                             transformPoint({rect.left(), rect.bottom(), 0})};
    Vec2 lo{corners[0].x, corners[0].y};
    Vec2 hi = lo;
    for (const Vec3 &c : corners) {
        lo = {std::min(lo.x, c.x), std::min(lo.y, c.y)};
        hi = {std::max(hi.x, c.x), std::max(hi.y, c.y)};
    }
    return RectF::fromCorners(lo, hi);
}

bool nearlyEqual(const Mat4 &a, const Mat4 &b, float tolerance) noexcept {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (std::abs(a(r, c) - b(r, c)) > tolerance) {
                return false;
            }
        }
    }
    return true;
}

} // namespace cfw
