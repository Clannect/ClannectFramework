// Vector types: arithmetic, products, normalisation of zero vectors.

#include "cfw/core/Vec2.h"
#include "cfw/core/Vec3.h"
#include "cfw/core/Vec4.h"

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

static_assert(Vec3{1, 2, 3} + Vec3{1, 1, 1} == Vec3{2, 3, 4}, "Vec3 arithmetic is constexpr");
static_assert(Vec3{1, 0, 0}.cross(Vec3{0, 1, 0}) == Vec3{0, 0, 1}, "right-handed cross product");

void vec2Arithmetic() {
    checkEqual(Vec2{1, 2} * 2.0f, Vec2{2, 4}, "scalar multiply");
    checkEqual(Vec2{3, 4}.scaled({2, 0.5f}), Vec2{6, 2}, "component-wise scale");
    checkNear(Vec2{3, 4}.length(), 5.0, 1e-6, "3-4-5 triangle");
    checkEqual(Vec2{}.normalized(), Vec2{}, "normalising zero gives zero, not NaN");
    checkEqual(Vec2i{2, 3}.toVec2(), Vec2{2, 3}, "integer to float");
}

void vec3Products() {
    checkNear(Vec3{1, 2, 3}.dot({4, 5, 6}), 32.0, 1e-6, "dot product");
    check(nearlyEqual(Vec3{0, 3, 4}.normalized(), Vec3{0, 0.6f, 0.8f}), "normalise");
    checkEqual(-Vec3{1, -2, 3}, Vec3{-1, 2, -3}, "negate");
}

void vec4Basics() {
    checkEqual(Vec4{1, 2, 3, 4}.xyz(), Vec3{1, 2, 3}, "xyz swizzle");
    checkNear(Vec4{1, 1, 1, 1}.dot({1, 2, 3, 4}), 10.0, 1e-6, "dot product");
}

} // namespace

int main() {
    vec2Arithmetic();
    vec3Products();
    vec4Basics();
    return cfw::test::finish("MathTest");
}
