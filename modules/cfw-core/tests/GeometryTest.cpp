// RectF/Recti edge rules, and Transform2D including the perspective quad
// mapping used to project interfaces onto surfaces. Expected quadToQuad
// results were produced by the Qt build (QTransform::quadToQuad, Qt 6.8.3).

#include "cfw/core/Rect.h"
#include "cfw/core/Transform2D.h"

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

void checkPoint(Vec2 actual, double x, double y, double tolerance, const char *message) {
    checkNear(actual.x, x, tolerance, message);
    checkNear(actual.y, y, tolerance, message);
}

void rectEdgesAreHalfOpen() {
    const RectF r{0, 0, 10, 10};
    check(r.contains(Vec2{0, 0}), "top-left corner is inside");
    check(!r.contains(Vec2{10, 5}), "right edge is outside");
    check(!r.intersects(RectF{10, 0, 5, 5}), "touching rectangles do not intersect");
    check(RectF{0, 0, 0, 5}.isEmpty(), "zero width is empty");
    check(RectF{0, 0, -1, 5}.isEmpty(), "negative width is empty");
}

void rectSetOperations() {
    const RectF a{0, 0, 10, 10};
    const RectF b{5, 5, 10, 10};
    checkEqual(a.intersected(b), RectF{5, 5, 5, 5}, "intersection");
    checkEqual(a.united(b), RectF{0, 0, 15, 15}, "union");
    checkEqual(a.united(RectF{}), a, "union with empty is unchanged");
    checkEqual(a.intersected(RectF{20, 20, 1, 1}), RectF{}, "disjoint intersection is empty");
    checkEqual(a.grownBy(1, 2, 3, 4), RectF{-1, -2, 14, 16}, "grownBy moves each edge");
    checkEqual(RectF::fromCorners({10, 0}, {0, 10}), a, "fromCorners normalises");
    checkEqual(a.center(), Vec2{5, 5}, "center");
    checkEqual(Recti{0, 0, 4, 4}.intersected(Recti{2, 2, 4, 4}), (Recti{2, 2, 2, 2}), "integer intersection");
}

void affineTransforms() {
    const Transform2D t = Transform2D::translation(10, 20).then(Transform2D::scaling(2, 3));
    checkPoint(t.map({1, 1}), 22, 63, 1e-9, "then() applies left to right");
    checkPoint(Transform2D::rotation(30).map({10, 0}), 8.66025404, 5, 1e-6, "rotation matches the renderer");
    const Transform2D quarter = Transform2D::rotation(90);
    checkEqual(quarter(0, 0), 0.0, "quarter turns are exact (cos)");
    checkEqual(quarter(1, 0), 1.0, "quarter turns are exact (sin)");
    const auto inverse = t.inverse();
    check(inverse.has_value(), "affine transform is invertible");
    checkPoint(inverse->map(t.map({3, 4})), 3, 4, 1e-6, "inverse undoes the transform");
    check(!Transform2D::scaling(0, 1).inverse().has_value(), "singular transform has no inverse");
    checkEqual(Transform2D::translation(5, 5).mapRect(RectF{0, 0, 2, 2}), RectF{5, 5, 2, 2}, "mapRect");
}

void quadToQuadMatchesTheSurfaceProjection() {
    const std::array<Vec2, 4> from{Vec2{0, 0}, Vec2{100, 0}, Vec2{100, 50}, Vec2{0, 50}};
    const std::array<Vec2, 4> to{Vec2{10, 20}, Vec2{210, 40}, Vec2{180, 160}, Vec2{30, 120}};
    const auto t = Transform2D::quadToQuad(from, to);
    check(t.has_value(), "quadToQuad succeeds");
    check(!t->isAffine(), "a trapezoid needs perspective");
    checkPoint(t->map({0, 0}), 10, 20, 1e-3, "corner 0");
    checkPoint(t->map({100, 50}), 180, 160, 1e-3, "corner 2");
    checkPoint(t->map({50, 25}), 95.8762887, 90.7216495, 1e-3, "centre matches the Qt build");
    checkPoint(t->map({25, 40}), 59.5554558, 111.55815, 1e-3, "interior point matches the Qt build");

    const std::array<Vec2, 4> parallelogram{Vec2{0, 0}, Vec2{10, 0}, Vec2{15, 5}, Vec2{5, 5}};
    const auto affine = Transform2D::squareToQuad(parallelogram);
    check(affine.has_value() && affine->isAffine(), "a parallelogram maps affinely");

    const std::array<Vec2, 4> collapsed{Vec2{0, 0}, Vec2{0, 0}, Vec2{0, 0}, Vec2{0, 0}};
    check(!Transform2D::quadToQuad(from, collapsed).has_value(), "degenerate quad is refused, not NaN");
}

} // namespace

int main() {
    rectEdgesAreHalfOpen();
    rectSetOperations();
    affineTransforms();
    quadToQuadMatchesTheSurfaceProjection();
    return cfw::test::finish("GeometryTest");
}
