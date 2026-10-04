#pragma once

#include <ostream>

#include "cfw/core/Vec3.h"

namespace cfw {

class Mat3;

// A rotation quaternion (w + xi + yj + zk). Rotations compose right to left:
// (a * b).rotate(v) == a.rotate(b.rotate(v)).
//
// Euler angles are degrees, as stored in scene files and shown in the
// Properties panel, as (pitch about X, yaw about Y, roll about Z), applied
// roll first, then pitch, then yaw: q = yaw * pitch * roll. Existing Clannect
// scenes were authored with exactly this convention, so it must not change.
//
// Threads: a plain value type. Allocates: nothing.
struct Quat {
    float w = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    [[nodiscard]] static constexpr Quat identity() noexcept { return {}; }

    // Rotation of `degrees` about `axis` (need not be normalised). A zero axis
    // gives the identity.
    [[nodiscard]] static Quat fromAxisAngle(Vec3 axis, float degrees) noexcept;
    [[nodiscard]] static Quat fromEulerDegrees(Vec3 pitchYawRoll) noexcept;
    // Rotation matrix to quaternion. `m` must be orthonormal.
    [[nodiscard]] static Quat fromRotationMatrix(const Mat3 &m) noexcept;
    // Rotation whose columns are the given orthonormal axes: the inverse of
    // toRotationMatrix(). Axes that are orthonormal only as far as float
    // arithmetic made them (normalised vectors and their cross products)
    // give a unit quaternion to within 2e-6, whose matrix gives the axes
    // back to within 3e-6. Axes that are further off give a quaternion that
    // is off by about as much and is not normalised: normalise the axes, or
    // the result.
    [[nodiscard]] static Quat fromAxes(Vec3 xAxis, Vec3 yAxis, Vec3 zAxis) noexcept;
    // Rotation mapping +Z to `direction`, keeping `up` as close to +Y as
    // possible. Zero direction gives the identity; `up` collinear with the
    // direction falls back to the shortest arc.
    [[nodiscard]] static Quat fromDirection(Vec3 direction, Vec3 up) noexcept;
    // The shortest rotation taking direction `from` to direction `to`.
    [[nodiscard]] static Quat rotationTo(Vec3 from, Vec3 to) noexcept;
    // Spherical interpolation along the shorter arc; t in [0, 1].
    [[nodiscard]] static Quat slerp(Quat a, Quat b, float t) noexcept;

    // (pitch, yaw, roll) in degrees; inverse of fromEulerDegrees. At gimbal lock
    // (pitch = ±90°) roll is reported as 0 and folded into yaw. Away from it
    // the angles round-trip: pitch within ±90 comes back to a hundredth of a
    // degree (up to 89°), yaw and roll likewise, in (-180, 180]. Angles
    // outside those ranges come back as the same rotation inside them.
    [[nodiscard]] Vec3 toEulerDegrees() const noexcept;
    [[nodiscard]] Mat3 toRotationMatrix() const noexcept;

    [[nodiscard]] Vec3 rotate(Vec3 v) const noexcept;
    [[nodiscard]] constexpr Quat conjugated() const noexcept { return {w, -x, -y, -z}; }
    [[nodiscard]] constexpr float lengthSquared() const noexcept { return w * w + x * x + y * y + z * z; }
    [[nodiscard]] Quat normalized() const noexcept;
    [[nodiscard]] constexpr float dot(Quat o) const noexcept { return w * o.w + x * o.x + y * o.y + z * o.z; }

    friend constexpr Quat operator*(Quat a, Quat b) noexcept {
        return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z, a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x};
    }
    friend constexpr bool operator==(Quat a, Quat b) noexcept = default;

    friend std::ostream &operator<<(std::ostream &out, Quat q) {
        return out << "Quat(" << q.w << "; " << q.x << ", " << q.y << ", " << q.z << ')';
    }
};

// Same rotation within `tolerance` (q and -q are the same rotation).
[[nodiscard]] bool nearlyEqual(Quat a, Quat b, float tolerance = 1e-5f) noexcept;

} // namespace cfw
