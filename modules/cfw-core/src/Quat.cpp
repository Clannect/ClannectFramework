#include "cfw/core/Quat.h"

#include <algorithm>
#include <cmath>

#include "cfw/core/Mat3.h"
#include "cfw/core/MathUtil.h"

namespace cfw {

namespace {

Quat scaled(Quat q, float s) noexcept { return {q.w * s, q.x * s, q.y * s, q.z * s}; }
Quat added(Quat a, Quat b) noexcept { return {a.w + b.w, a.x + b.x, a.y + b.y, a.z + b.z}; }

} // namespace

Quat Quat::fromAxisAngle(Vec3 axis, float degrees) noexcept {
    const float length = axis.length();
    if (!nearlyZero(length - 1.0f) && !nearlyZero(length)) {
        axis /= length;
    }
    const float half = degreesToRadians(degrees / 2.0f);
    const float s = std::sin(half);
    return Quat{std::cos(half), axis.x * s, axis.y * s, axis.z * s}.normalized();
}

Quat Quat::fromEulerDegrees(Vec3 pitchYawRoll) noexcept {
    // q = yaw(Y) * pitch(X) * roll(Z), expanded.
    const float pitch = degreesToRadians(pitchYawRoll.x) * 0.5f;
    const float yaw = degreesToRadians(pitchYawRoll.y) * 0.5f;
    const float roll = degreesToRadians(pitchYawRoll.z) * 0.5f;
    const float c1 = std::cos(yaw), s1 = std::sin(yaw);
    const float c2 = std::cos(roll), s2 = std::sin(roll);
    const float c3 = std::cos(pitch), s3 = std::sin(pitch);
    const float c1c2 = c1 * c2;
    const float s1s2 = s1 * s2;
    return {c1c2 * c3 + s1s2 * s3, c1c2 * s3 + s1s2 * c3, s1 * c2 * c3 - c1 * s2 * s3, c1 * s2 * c3 - s1 * c2 * s3};
}

Vec3 Quat::toEulerDegrees() const noexcept {
    float xx = x * x, xy = x * y, xz = x * z, xw = x * w;
    float yy = y * y, yz = y * z, yw = y * w;
    float zz = z * z, zw = z * w;
    const float len = lengthSquared();
    if (!nearlyZero(len - 1.0f) && !nearlyZero(len)) {
        xx /= len; xy /= len; xz /= len; xw /= len;
        yy /= len; yz /= len; yw /= len;
        zz /= len; zw /= len;
    }

    // Within 1e-5 of ±1 counts as gimbal lock (about 0.26° from straight up or
    // down). Scenes saved near ±90° pitch rely on this exact threshold to decode
    // to the same angles they were saved with.
    const float sinPitch = -2.0f * (yz - xw);
    const bool locked = nearlyZero(1.0f - std::abs(sinPitch));
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
    if (!locked) {
        // Clamped: rounding can push the argument just past ±1 (asin -> NaN).
        pitch = std::asin(std::clamp(sinPitch, -1.0f, 1.0f));
        yaw = std::atan2(2.0f * (xz + yw), 1.0f - 2.0f * (xx + yy));
        roll = std::atan2(2.0f * (xy + zw), 1.0f - 2.0f * (xx + zz));
    } else {
        // Straight up or down: yaw and roll turn about the same axis, so only
        // their combination is defined. Report roll as 0 and the whole turn as
        // yaw, read from the quaternion directly (stable right up to the pole).
        pitch = sinPitch > 0.0f ? kPi / 2.0f : -kPi / 2.0f;
        yaw = 2.0f * std::atan2(y, w);
    }
    return {radiansToDegrees(pitch), radiansToDegrees(yaw), radiansToDegrees(roll)};
}

Quat Quat::fromRotationMatrix(const Mat3 &m) noexcept {
    // Shepperd's method: pick the largest diagonal term for stability.
    const float trace = m(0, 0) + m(1, 1) + m(2, 2);
    if (trace > 0.00000001f) {
        const float s = 2.0f * std::sqrt(trace + 1.0f);
        return {0.25f * s, (m(2, 1) - m(1, 2)) / s, (m(0, 2) - m(2, 0)) / s, (m(1, 0) - m(0, 1)) / s};
    }
    constexpr int next[3] = {1, 2, 0};
    int i = 0;
    if (m(1, 1) > m(0, 0)) {
        i = 1;
    }
    if (m(2, 2) > m(i, i)) {
        i = 2;
    }
    const int j = next[i];
    const int k = next[j];
    const float s = 2.0f * std::sqrt(m(i, i) - m(j, j) - m(k, k) + 1.0f);
    float axis[3] = {};
    axis[i] = 0.25f * s;
    axis[j] = (m(j, i) + m(i, j)) / s;
    axis[k] = (m(k, i) + m(i, k)) / s;
    return {(m(k, j) - m(j, k)) / s, axis[0], axis[1], axis[2]};
}

Quat Quat::fromAxes(Vec3 xAxis, Vec3 yAxis, Vec3 zAxis) noexcept {
    return fromRotationMatrix(Mat3::fromColumns(xAxis, yAxis, zAxis));
}

Quat Quat::fromDirection(Vec3 direction, Vec3 up) noexcept {
    if (nearlyZero(direction.x) && nearlyZero(direction.y) && nearlyZero(direction.z)) {
        return {};
    }
    const Vec3 zAxis = direction.normalized();
    Vec3 xAxis = up.cross(zAxis);
    if (nearlyZero(xAxis.lengthSquared())) {
        // `up` is collinear with the direction (or zero): take the shortest arc.
        return rotationTo({0.0f, 0.0f, 1.0f}, zAxis);
    }
    xAxis = xAxis.normalized();
    const Vec3 yAxis = zAxis.cross(xAxis);
    return fromAxes(xAxis, yAxis, zAxis);
}

Quat Quat::rotationTo(Vec3 from, Vec3 to) noexcept {
    const Vec3 v0 = from.normalized();
    const Vec3 v1 = to.normalized();
    float d = v0.dot(v1) + 1.0f;
    if (nearlyZero(d)) {
        // Opposite directions: any perpendicular axis works; pick a stable one.
        Vec3 axis = Vec3{1.0f, 0.0f, 0.0f}.cross(v0);
        if (nearlyZero(axis.lengthSquared())) {
            axis = Vec3{0.0f, 1.0f, 0.0f}.cross(v0);
        }
        axis = axis.normalized();
        return {0.0f, axis.x, axis.y, axis.z};
    }
    d = std::sqrt(2.0f * d);
    const Vec3 axis = v0.cross(v1) / d;
    return Quat{d * 0.5f, axis.x, axis.y, axis.z}.normalized();
}

Quat Quat::slerp(Quat a, Quat b, float t) noexcept {
    if (t <= 0.0f) {
        return a;
    }
    if (t >= 1.0f) {
        return b;
    }
    float d = a.dot(b);
    if (d < 0.0f) {
        b = scaled(b, -1.0f);
        d = -d;
    }
    float factorA = 1.0f - t;
    float factorB = t;
    if (1.0f - d > 0.0000001f) {
        const float angle = std::acos(d);
        const float sinAngle = std::sin(angle);
        if (sinAngle > 0.0000001f) {
            factorA = std::sin((1.0f - t) * angle) / sinAngle;
            factorB = std::sin(t * angle) / sinAngle;
        }
    }
    return added(scaled(a, factorA), scaled(b, factorB));
}

Mat3 Quat::toRotationMatrix() const noexcept {
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float xw = x * w, yw = y * w, zw = z * w;
    return Mat3::fromColumns({1.0f - 2.0f * (yy + zz), 2.0f * (xy + zw), 2.0f * (xz - yw)},
                             {2.0f * (xy - zw), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + xw)},
                             {2.0f * (xz + yw), 2.0f * (yz - xw), 1.0f - 2.0f * (xx + yy)});
}

Vec3 Quat::rotate(Vec3 v) const noexcept {
    const Quat r = *this * Quat{0.0f, v.x, v.y, v.z} * conjugated();
    return {r.x, r.y, r.z};
}

Quat Quat::normalized() const noexcept {
    const float len = std::sqrt(lengthSquared());
    if (nearlyZero(len - 1.0f)) {
        return *this;
    }
    if (nearlyZero(len)) {
        return {}; // a zero quaternion is not a rotation; identity is the safe answer
    }
    return scaled(*this, 1.0f / len);
}

bool nearlyEqual(Quat a, Quat b, float tolerance) noexcept {
    const auto close = [tolerance](Quat p, Quat q) {
        return std::abs(p.w - q.w) <= tolerance && std::abs(p.x - q.x) <= tolerance &&
               std::abs(p.y - q.y) <= tolerance && std::abs(p.z - q.z) <= tolerance;
    };
    return close(a, b) || close(a, scaled(b, -1.0f));
}

} // namespace cfw
