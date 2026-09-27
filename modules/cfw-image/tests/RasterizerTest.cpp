// Scan conversion and paths: coverage is exact for pixel-aligned and
// half-pixel edges, sums to the analytic area for curves and slanted edges,
// follows both fill rules, clips correctly on every side (including the
// winding of edges left of the target), and survives hostile coordinates.

#include "cfw/image/Rasterizer.h"

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

// Rasterises into a width x height coverage grid.
struct Grid {
    int width;
    int height;
    std::vector<std::uint8_t> a;

    Grid(int w, int h) : width(w), height(h), a(static_cast<std::size_t>(w * h), 0) {}
    std::uint8_t at(int x, int y) const { return a[static_cast<std::size_t>(y * width + x)]; }
    double total() const {
        double sum = 0;
        for (const std::uint8_t v : a) {
            sum += v / 255.0;
        }
        return sum;
    }
};

Grid fill(int w, int h, const Path &path, FillRule rule = FillRule::NonZero, const Transform2D &t = {}) {
    Grid g(w, h);
    Rasterizer r;
    r.reset(w, h);
    r.addPath(path, t);
    bool ordered = true;
    int lastY = -1;
    r.sweep(rule, [&](int y, int x, Span<const std::uint8_t> coverage) {
        ordered = ordered && y > lastY && y >= 0 && y < h && x >= 0 && x + static_cast<int>(coverage.size()) <= w;
        lastY = y;
        for (std::size_t i = 0; i < coverage.size(); ++i) {
            g.a[static_cast<std::size_t>(y * w) + static_cast<std::size_t>(x) + i] = coverage[i];
        }
    });
    check(ordered, "rows arrive in order, inside the target");
    return g;
}

Path rect(float x, float y, float w, float h) {
    Path p;
    p.addRect({x, y, w, h});
    return p;
}

void alignedRectanglesAreExact() {
    const Grid g = fill(10, 10, rect(2, 3, 5, 4));
    bool exact = true;
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 10; ++x) {
            const bool inside = x >= 2 && x < 7 && y >= 3 && y < 7;
            exact = exact && g.at(x, y) == (inside ? 255 : 0);
        }
    }
    check(exact, "a pixel-aligned rectangle covers exactly its pixels, fully");

    const Grid half = fill(10, 10, rect(2.5f, 3, 5, 4));
    checkEqual(static_cast<int>(half.at(2, 4)), 128, "a half-covered pixel is 128");
    checkEqual(static_cast<int>(half.at(7, 4)), 128, "on both sides");
    checkEqual(static_cast<int>(half.at(4, 4)), 255, "the inside is full");
    const Grid quarter = fill(10, 10, rect(2.5f, 3.5f, 5, 4));
    checkEqual(static_cast<int>(quarter.at(2, 3)), 64, "a quarter-covered corner is 64");

    // Direction does not matter for a single shape.
    Path reversed;
    reversed.addPolygon(std::vector<Vec2>{{2, 3}, {2, 7}, {7, 7}, {7, 3}});
    check(fill(10, 10, reversed).a == g.a, "counter-clockwise gives the same coverage");
}

void areasMatchGeometry() {
    // A circle of radius 20: pi r^2.
    Path circle;
    circle.addEllipse({10.3f, 12.7f, 40, 40});
    const Grid c = fill(64, 64, circle);
    checkNear(c.total(), std::numbers::pi * 400.0, std::numbers::pi * 400.0 * 0.005, "circle area within 0.5% (Qt: 0.57%)");
    // A slanted triangle: exact area (0.5 * base * height) up to 8-bit rounding.
    Path tri;
    tri.addPolygon(std::vector<Vec2>{{3.2f, 1.7f}, {60.9f, 20.3f}, {17.4f, 55.1f}});
    const double expected = 0.5 * std::abs((60.9 - 3.2) * (55.1 - 1.7) - (17.4 - 3.2) * (20.3 - 1.7));
    checkNear(fill(64, 64, tri).total(), expected, expected * 0.002, "triangle area within 0.2%");
    // Rounded rectangle: w h - (4 - pi) r^2.
    Path rounded;
    rounded.addRoundedRect({4, 4, 50, 30}, 8, 8);
    const double rr = 50.0 * 30.0 - (4.0 - std::numbers::pi) * 64.0;
    checkNear(fill(64, 64, rounded).total(), rr, rr * 0.002, "rounded rectangle area within 0.2% (Qt: 0.06%)");
}

void fillRules() {
    // Two overlapping squares drawn the same way round.
    Path two = rect(2, 2, 6, 6);
    two.addPath(rect(5, 5, 6, 6));
    const Grid nonZero = fill(14, 14, two, FillRule::NonZero);
    const Grid evenOdd = fill(14, 14, two, FillRule::EvenOdd);
    checkEqual(static_cast<int>(nonZero.at(6, 6)), 255, "non-zero: the overlap is inside");
    checkEqual(static_cast<int>(evenOdd.at(6, 6)), 0, "even-odd: the overlap is a hole");
    checkEqual(static_cast<int>(evenOdd.at(3, 3)), 255, "the rest is inside either way");
    // A square with an inner square drawn the other way round: a hole in both.
    Path donut = rect(1, 1, 10, 10);
    donut.addPolygon(std::vector<Vec2>{{4, 4}, {4, 8}, {8, 8}, {8, 4}});
    checkEqual(static_cast<int>(fill(12, 12, donut, FillRule::NonZero).at(5, 5)), 0, "opposite winding cancels");
}

void clipsOnEverySide() {
    // Mostly outside on the left: the winding of the off-target part counts.
    const Grid left = fill(10, 10, rect(-100, 2, 104.5f, 3));
    checkEqual(static_cast<int>(left.at(0, 3)), 255, "covered from the left edge");
    checkEqual(static_cast<int>(left.at(3, 3)), 255, "up to the edge");
    checkEqual(static_cast<int>(left.at(4, 3)), 128, "with the half pixel");
    checkEqual(static_cast<int>(left.at(5, 3)), 0, "and nothing after");
    // Covering everything, edges outside on all sides.
    const Grid all = fill(8, 8, rect(-1e6f, -1e6f, 2e6f, 2e6f));
    bool full = true;
    for (const std::uint8_t v : all.a) {
        full = full && v == 255;
    }
    check(full, "a huge rectangle around the target covers all of it");
    // Entirely outside.
    checkEqual(fill(8, 8, rect(20, 20, 5, 5)).total(), 0.0, "outside on the right and below: nothing");
    checkEqual(fill(8, 8, rect(-20, -20, 5, 5)).total(), 0.0, "outside on the left and above: nothing");
    // A shape crossing the right edge.
    const Grid right = fill(10, 4, rect(6.5f, 0, 100, 4));
    checkEqual(static_cast<int>(right.at(6, 1)), 128, "partly in from the right");
    checkEqual(static_cast<int>(right.at(9, 1)), 255, "to the last column");
}

void transformsAndHostileInput() {
    // Scaled 2x, a 3x2 rectangle covers 6x4 pixels.
    const Grid scaled = fill(12, 12, rect(1, 1, 3, 2), FillRule::NonZero, Transform2D::scaling(2, 2));
    checkNear(scaled.total(), 24.0, 1e-9, "a scaled rectangle covers the scaled area");
    // Perspective (quad-to-quad) maps a square onto a trapezoid.
    const auto quad = Transform2D::quadToQuad({Vec2{0, 0}, Vec2{1, 0}, Vec2{1, 1}, Vec2{0, 1}},
                                              {Vec2{10, 5}, Vec2{50, 5}, Vec2{60, 45}, Vec2{0, 45}});
    check(quad.has_value(), "the perspective transform exists");
    Path unit = rect(0, 0, 1, 1);
    const double trapezoid = 0.5 * (40.0 + 60.0) * 40.0;
    checkNear(fill(64, 64, unit, FillRule::NonZero, *quad).total(), trapezoid, trapezoid * 0.002,
              "a perspective-mapped square covers the trapezoid");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    Path bad;
    bad.addPolygon(std::vector<Vec2>{{1, 1}, {nan, 3}, {inf, -inf}, {5, 5}});
    (void)fill(8, 8, bad);
    check(true, "non-finite coordinates do not crash");
    Path many;
    for (int i = 0; i < 2000; ++i) {
        many.addRect({static_cast<float>(i % 50) - 20.0f, static_cast<float>(i % 37) - 10.0f, 30, 25});
    }
    const Grid g = fill(32, 32, many);
    check(g.total() > 0, "thousands of overlapping shapes rasterise");
    Rasterizer r;
    r.reset(0, 0);
    r.addPath(rect(0, 0, 5, 5));
    bool called = false;
    r.sweep(FillRule::NonZero, [&](int, int, Span<const std::uint8_t>) { called = true; });
    check(!called, "a zero-size target produces nothing");
}

void pathsBehaveLikeQPainterPath() {
    Path p;
    p.lineTo({5, 5}); // starts at the origin, as QPainterPath does
    check(p.verbs().size() == 2 && p.points()[0] == Vec2{0, 0}, "drawing on an empty path starts at (0, 0)");
    p.close();
    p.lineTo({1, 9}); // continues from the closed sub-path's start
    checkEqual(p.points()[p.points().size() - 2].x, 0.0f, "after close(), drawing continues from the start");
    Path arc;
    arc.moveTo({20, 10});
    arc.arcTo({0, 0, 20, 20}, 0, 90); // counter-clockwise on screen: up to 12 o'clock
    const Vec2 end = arc.currentPoint();
    checkNear(end.x, 10.0, 1e-4, "arc ends at 12 o'clock (x)");
    checkNear(end.y, 0.0, 1e-4, "arc ends at 12 o'clock (y, screen up)");
    const RectF b = rect(1, 2, 3, 4).controlBounds();
    check(b.x == 1 && b.y == 2 && b.width == 3 && b.height == 4, "bounds");
    // Flattening respects the tolerance.
    Path c;
    c.addEllipse({0, 0, 200, 200});
    for (const float tolerance : {1.0f, 0.1f}) {
        double worst = 0;
        for (const Path::Polyline &poly : c.flatten(tolerance)) {
            for (std::size_t i = 0; i + 1 < poly.points.size(); ++i) {
                // Mid-chord distance from the circle.
                const Vec2 a = poly.points[i];
                const Vec2 bb = poly.points[i + 1];
                const double mx = (a.x + bb.x) / 2 - 100;
                const double my = (a.y + bb.y) / 2 - 100;
                worst = std::max(worst, 100 - std::sqrt(mx * mx + my * my));
            }
        }
        check(worst <= tolerance + 0.03, "flattened chords stay within the tolerance");
    }
}

} // namespace

int main() {
    alignedRectanglesAreExact();
    areasMatchGeometry();
    fillRules();
    clipsOnEverySide();
    transformsAndHostileInput();
    pathsBehaveLikeQPainterPath();
    return cfw::test::finish("RasterizerTest");
}
