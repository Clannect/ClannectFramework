// Quat and Mat4 must reproduce the rotation and projection conventions that
// existing Clannect scenes were authored with. The expected numbers below were
// produced by the Qt build of the engine (QQuaternion / QMatrix4x4, Qt 6.8.3)
// and are pinned here, so the conventions survive after Qt is gone.

#include "cfw/core/Mat4.h"
#include "cfw/core/Quat.h"

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkNear;

namespace {

void checkQuat(Quat actual, Quat expected, const char *message) {
    check(nearlyEqual(actual, expected, 2e-6f), message);
    if (!nearlyEqual(actual, expected, 2e-6f)) {
        std::printf("      got %s, expected %s\n", cfw::test::describe(actual).c_str(),
                    cfw::test::describe(expected).c_str());
    }
}

void checkVec(Vec3 actual, Vec3 expected, float tolerance, const char *message) {
    check(nearlyEqual(actual, expected, tolerance), message);
    if (!nearlyEqual(actual, expected, tolerance)) {
        std::printf("      got %s, expected %s\n", cfw::test::describe(actual).c_str(),
                    cfw::test::describe(expected).c_str());
    }
}

void checkMat(const Mat4 &actual, const float (&expectedRows)[16], const char *message) {
    const Mat4 expected = Mat4::fromRows({expectedRows[0], expectedRows[1], expectedRows[2], expectedRows[3]},
                                         {expectedRows[4], expectedRows[5], expectedRows[6], expectedRows[7]},
                                         {expectedRows[8], expectedRows[9], expectedRows[10], expectedRows[11]},
                                         {expectedRows[12], expectedRows[13], expectedRows[14], expectedRows[15]});
    check(nearlyEqual(actual, expected, 2e-5f), message);
    if (!nearlyEqual(actual, expected, 2e-5f)) {
        std::printf("      got %s\n      expected %s\n", cfw::test::describe(actual).c_str(),
                    cfw::test::describe(expected).c_str());
    }
}

void eulerAnglesMatchTheSceneConvention() {
    checkQuat(Quat::fromEulerDegrees({30, 45, 60}), {0.8223631f, 0.3919038f, 0.2005621f, 0.3604234f}, "euler(30,45,60)");
    checkQuat(Quat::fromEulerDegrees({-10, 170, 5}), {0.0829543f, 0.03569916f, 0.9917907f, 0.09052867f},
              "euler(-10,170,5)");
    checkQuat(Quat::fromEulerDegrees({90, 20, 30}), {0.704416f, 0.704416f, -0.06162841f, 0.06162841f},
              "euler(90,20,30)");
}

void eulerRoundTripsIncludingGimbalLock() {
    checkVec(Quat::fromEulerDegrees({30, 45, 60}).toEulerDegrees(), {30, 45, 60}, 1e-3f, "round-trip away from lock");
    checkVec(Quat::fromEulerDegrees({90, 20, 30}).toEulerDegrees(), {90, -10, 0}, 1e-3f,
             "looking straight up folds roll into yaw (yaw - roll)");
    checkVec(Quat::fromEulerDegrees({-90, 20, 30}).toEulerDegrees(), {-90, 50, 0}, 1e-3f,
             "looking straight down folds roll into yaw (yaw + roll)");
    checkVec(Quat::fromEulerDegrees({89.9f, 20, 30}).toEulerDegrees(), {90, -9.95004f, 0}, 1e-3f,
             "within 0.26 degrees of vertical counts as locked");
    checkVec(Quat::fromEulerDegrees({89.7f, 20, 30}).toEulerDegrees(), {89.7f, 20, 30}, 5e-3f,
             "just outside the lock threshold decodes normally");
    // Every probe point the Qt build reported around both poles.
    checkVec(Quat::fromEulerDegrees({89.745f, 20, 30}).toEulerDegrees(), {90, -9.87273f, 0}, 2e-3f, "lock edge (up)");
    checkVec(Quat::fromEulerDegrees({89.99f, 20, 30}).toEulerDegrees(), {90, -9.995f, 0}, 2e-3f, "near pole (up)");
    checkVec(Quat::fromEulerDegrees({-89.99f, 20, 30}).toEulerDegrees(), {-90, 49.995f, 0}, 2e-3f, "near pole (down)");
    const Vec3 e = Quat{1.0000001f, 0, 0, 0}.toEulerDegrees();
    check(e.x == e.x && e.y == e.y && e.z == e.z, "slightly denormalised input never produces NaN");
}

void axisAngleAndRotation() {
    checkQuat(Quat::fromAxisAngle({1, 2, 3}, 75), {0.7933533f, 0.1626983f, 0.3253967f, 0.488095f}, "axis-angle");
    checkVec(Quat::fromEulerDegrees({30, 45, 60}).rotate({1, 2, 3}), {1.625665f, 0.1160257f, 3.368048f}, 1e-5f,
             "rotate a vector");
    checkQuat(Quat::fromAxisAngle({0, 0, 0}, 90), Quat::identity(), "zero axis gives identity");
    const Quat a = Quat::fromEulerDegrees({10, 20, 30});
    const Quat b = Quat::fromEulerDegrees({-40, 5, 70});
    checkVec((a * b).rotate({1, 2, 3}), a.rotate(b.rotate({1, 2, 3})), 1e-5f, "composition applies right first");
}

void directionalConstructors() {
    checkQuat(Quat::rotationTo({1, 0, 0}, {0, 1, 1}), {0.7071068f, 0, -0.5000001f, 0.5000001f}, "rotationTo");
    checkQuat(Quat::rotationTo({1, 0, 0}, {-1, 0, 0}), {0, 0, 0, -1}, "rotationTo opposite picks a stable axis");
    checkQuat(Quat::fromDirection({1, 1, -1}, {0, 1, 0}), {0.3647052f, -0.1159169f, 0.8804762f, 0.2798482f},
              "fromDirection");
    checkQuat(Quat::fromDirection({0, 1, 0}, {0, 1, 0}), {0.7071068f, -0.7071068f, 0, 0},
              "fromDirection with collinear up");
    checkQuat(Quat::slerp(Quat::fromEulerDegrees({0, 10, 0}), Quat::fromEulerDegrees({40, 100, -20}), 0.3f),
              {0.9384243f, 0.03126612f, 0.3209458f, -0.1239999f}, "slerp");
}

void matrixRoundTrip() {
    const Quat q = Quat::fromEulerDegrees({12, -34, 56});
    checkQuat(Quat::fromRotationMatrix(q.toRotationMatrix()), q, "quat -> matrix -> quat");
    const Quat flipped = Quat::fromAxisAngle({0, 1, 0}, 179.0f); // trace < 0 branch
    checkQuat(Quat::fromRotationMatrix(flipped.toRotationMatrix()), flipped, "large-angle matrix round-trip");
}

void projectionsMatchTheRenderer() {
    checkMat(Mat4::perspective(60, 16.0f / 9.0f, 0.1f, 1000.0f),
             {0.9742786f, 0, 0, 0, 0, 1.732051f, 0, 0, 0, 0, -1.0002f, -0.20002f, 0, 0, -1, 0}, "perspective");
    checkMat(Mat4::lookAt({3, 4, 5}, {0, 1, 0}, {0, 1, 0}),
             {0.8574929f, 0, -0.5144957f, 0, -0.2353796f, 0.8892118f, -0.3922994f, -0.8892117f, 0.4574957f,
              0.4574957f, 0.7624929f, -7.014935f, 0, 0, 0, 1},
             "lookAt");
    checkMat(Mat4::ortho(-2, 6, -1, 3, 0.5f, 50),
             {0.25f, 0, 0, -0.5f, 0, 0.5f, 0, -0.5f, 0, 0, -0.04040404f, -1.020202f, 0, 0, 0, 1}, "ortho");
    check(Mat4::perspective(0, 1, 1, 10) == Mat4(), "degenerate perspective is the identity");
}

void composedTransformsAndInverse() {
    const Mat4 trs = Mat4::translation({1, 2, 3}) * Mat4::rotation(Quat::fromEulerDegrees({30, 45, 60})) *
                     Mat4::scaling({2, 3, 4});
    checkMat(trs,
             {1.319479f, -1.306787f, 2.44949f, 1, 1.5f, 1.299038f, -2, 2, -0.09473443f, 2.367447f, 2.44949f, 3, 0, 0,
              0, 1},
             "translate * rotate * scale");
    const auto inverse = trs.inverse();
    check(inverse.has_value(), "TRS is invertible");
    checkMat(*inverse,
             {0.3298698f, 0.375f, -0.02368362f, -1.008819f, -0.1451986f, 0.1443376f, 0.2630497f, -0.9326258f,
              0.1530931f, -0.125f, 0.1530931f, -0.3623724f, 0, 0, 0, 1},
             "inverse");
    check(nearlyEqual(trs * *inverse, Mat4(), 1e-5f), "M * inverse(M) is the identity");
    check(!Mat4::scaling({1, 0, 1}).inverse().has_value(), "singular matrix has no inverse");

    checkVec(trs.transformPoint({0, 0, 0}), {1, 2, 3}, 1e-6f, "points get the translation");
    checkVec(trs.transformVector({0, 0, 0}), {0, 0, 0}, 1e-6f, "vectors do not");
    const Mat3 normals = Mat4::scaling({2, 1, 1}).normalMatrix();
    checkNear(normals(0, 0), 0.5, 1e-6, "normal matrix is the inverse transpose");
}

} // namespace

int main() {
    eulerAnglesMatchTheSceneConvention();
    eulerRoundTripsIncludingGimbalLock();
    axisAngleAndRotation();
    directionalConstructors();
    matrixRoundTrip();
    projectionsMatchTheRenderer();
    composedTransformsAndInverse();
    return cfw::test::finish("RotationTest");
}
