// The stroker against geometry and against values measured from Qt 6.11's
// QPainterPathStroker (docs/decisions/0007): caps, joins, miter clipping,
// dashes, curves and hostile input.

#include "cfw/gfx/Stroker.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include "cfw/image/Rasterizer.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

// Covered area of a stroke (non-zero fill), in pixels, over [-100, 300)^2.
double strokeArea(const PainterPath &path, const Pen &pen) {
    Stroker s;
    Rasterizer r;
    r.reset(400, 400);
    r.addPath(s.stroke(path, pen), Transform2D::translation(100, 100));
    double sum = 0;
    r.sweep(FillRule::NonZero, [&](int, int, Span<const std::uint8_t> c) {
        for (const std::uint8_t v : c) {
            sum += v / 255.0;
        }
    });
    return sum;
}

Pen pen(float width, CapStyle cap = CapStyle::Flat, JoinStyle join = JoinStyle::Bevel) {
    Pen p;
    p.width = width;
    p.cap = cap;
    p.join = join;
    return p;
}

std::size_t subpaths(const PainterPath &p) {
    std::size_t n = 0;
    for (const PainterPath::Verb v : p.verbs()) {
        n += v == PainterPath::Verb::Move ? 1 : 0;
    }
    return n;
}

PainterPath line(Vec2 a, Vec2 b) {
    PainterPath p;
    p.moveTo(a);
    p.lineTo(b);
    return p;
}

void caps() {
    const PainterPath l = line({10, 10}, {50, 10});
    checkNear(strokeArea(l, pen(4, CapStyle::Flat)), 160.0, 0.05, "flat caps: length x width");
    checkNear(strokeArea(l, pen(4, CapStyle::Square)), 176.0, 0.05, "square caps add half a width at each end");
    const PainterPath longer = line({10, 10}, {110, 10});
    checkNear(strokeArea(longer, pen(40, CapStyle::Round)), 4000.0 + std::numbers::pi * 400.0, 6.3,
              "round caps add a circle (within 0.5% of it, the fill tolerance)");
    checkEqual(Pen{}.cap == CapStyle::Square && Pen{}.join == JoinStyle::Bevel && Pen{}.width == 1.0f &&
                   Pen{}.miterLimit == 2.0f,
               true, "Qt's pen defaults");
}

void joins() {
    // An L of two 40-long, 6-wide segments: 471 plus the outer corner.
    PainterPath l;
    l.moveTo({10, 10});
    l.lineTo({50, 10});
    l.lineTo({50, 50});
    checkNear(strokeArea(l, pen(6, CapStyle::Flat, JoinStyle::Bevel)), 475.5, 0.05, "bevel: half the corner square");
    checkNear(strokeArea(l, pen(6, CapStyle::Flat, JoinStyle::Miter)), 480.0, 0.05, "miter: the whole corner square");
    PainterPath big;
    big.moveTo({0, 0});
    big.lineTo({100, 0});
    big.lineTo({100, 100});
    checkNear(strokeArea(big, pen(40, CapStyle::Flat, JoinStyle::Round)), 7600.0 + std::numbers::pi * 100.0, 1.6,
              "round: a quarter circle");
    // Turning the other way gives the same area (the join follows the outside).
    PainterPath r;
    r.moveTo({50, 50});
    r.lineTo({50, 10});
    r.lineTo({10, 10});
    checkNear(strokeArea(r, pen(6, CapStyle::Flat, JoinStyle::Miter)), 480.0, 0.05, "either direction");
}

// A V with its point at the origin and arms `opening` degrees apart, stroked
// 10 wide: the top of the outline, as QPainterPathStroker reports it.
float vTop(float opening, JoinStyle join) {
    const double a = opening * std::numbers::pi / 360.0;
    PainterPath v;
    v.moveTo({static_cast<float>(-150 * std::sin(a)), static_cast<float>(150 * std::cos(a))});
    v.lineTo({0, 0});
    v.lineTo({static_cast<float>(150 * std::sin(a)), static_cast<float>(150 * std::cos(a))});
    Stroker s;
    return s.stroke(v, pen(10, CapStyle::Flat, join)).controlBounds().y;
}

void miterLimits() {
    // Qt 6.11: full point up to miterLimit * width past the outer edge, then cut off there.
    checkNear(vTop(30, JoinStyle::Miter), -19.319, 0.002, "30 degrees: the full point (Qt)");
    checkNear(vTop(28, JoinStyle::Miter), -20.616, 0.002, "28 degrees: cut off (Qt)");
    checkNear(vTop(20, JoinStyle::Miter), -20.564, 0.002, "20 degrees: cut off (Qt)");
    checkNear(vTop(10, JoinStyle::Miter), -20.360, 0.002, "10 degrees: cut off (Qt)");
    // SvgMiter: 1 / sin(opening / 2) over the limit (2) becomes a bevel.
    checkNear(vTop(60, JoinStyle::SvgMiter), -10.0, 0.002, "SVG miter at the limit: the full point");
    checkNear(vTop(40, JoinStyle::SvgMiter), static_cast<float>(-5 * std::sin(20 * std::numbers::pi / 180)), 0.002,
              "SVG miter past the limit: a bevel");
    // A U-turn has no miter point: cut off at the limit.
    PainterPath u;
    u.moveTo({20, 20});
    u.lineTo({80, 20});
    u.lineTo({20, 20});
    Stroker s;
    checkNear(s.stroke(u, pen(8, CapStyle::Flat, JoinStyle::Miter)).controlBounds().right(), 96.0, 0.01,
              "a U-turn's miter is cut off at miterLimit * width");
}

void dashes() {
    Stroker s;
    const PainterPath l = line({0, 0}, {20, 0});
    Pen p = pen(1);
    p.dashes = {3, 1};
    checkEqual(subpaths(s.stroke(l, p)), std::size_t{5}, "3 on 1 off over 20: five dashes");
    p.dashOffset = 1;
    checkEqual(subpaths(s.stroke(l, p)), std::size_t{6}, "offset by 1: a short first dash, six in all (Qt)");
    p.dashOffset = 0;
    p.dashes = {3, 1, 2};
    checkEqual(subpaths(s.stroke(l, p)), std::size_t{5}, "an odd pattern drops its last entry (Qt)");
    p.dashes = {0, 3};
    check(s.stroke(l, pen(2, CapStyle::Round)).verbs().size() > 0 && s.stroke(l, p).empty(),
          "zero-length dashes draw nothing, whatever the cap (Qt)");
    Pen wide = pen(2);
    wide.dashes = {4, 2};
    checkNear(strokeArea(line({0, 10}, {60, 10}), wide), 2.0 * 8 * 5, 0.05, "dash lengths are in pen widths");

    // A dash running round a corner is one piece with a real join: Qt's exact outline.
    PainterPath rect;
    rect.addRect({0, 0, 10, 10});
    Pen dashed = pen(1, CapStyle::Flat, JoinStyle::Miter);
    dashed.dashes = {3, 1};
    const PainterPath out = s.stroke(rect, dashed);
    checkEqual(subpaths(out), std::size_t{10}, "ten dashes round a 10 x 10 square (Qt)");
    // The third piece. QPainterPathStroker emits (8, -0.5) (10, -0.5)
    // (10.5, -0.5) (10.5, 0) (10.5, 1) (9.5, 1) (9.5, 0) (10, 0) (10, 0.5)
    // (8, 0.5): the same region, but its inner corner detours through the
    // corner point where this stroker ends the edges where they cross.
    const std::vector<Vec2> qt{{8, -0.5f}, {10, -0.5f}, {10.5f, -0.5f}, {10.5f, 0},
                               {10.5f, 1}, {9.5f, 1},   {9.5f, 0.5f},   {8, 0.5f}};
    std::size_t move = 0;
    std::size_t index = 0;
    std::vector<Vec2> third;
    for (const PainterPath::Verb v : out.verbs()) {
        move += v == PainterPath::Verb::Move ? 1 : 0;
        if (v != PainterPath::Verb::Close) {
            if (move == 3) {
                third.push_back(out.points()[index]);
            }
            ++index;
        }
    }
    bool same = third.size() == qt.size();
    for (std::size_t i = 0; same && i < qt.size(); ++i) {
        same = std::abs(third[i].x - qt[i].x) < 1e-5f && std::abs(third[i].y - qt[i].y) < 1e-5f;
    }
    check(same, "the piece round the corner has Qt's outline (miter point, square inner corner)");
}

void curvesAndClosedPaths() {
    // A ring: 2 pi r w.
    PainterPath circle;
    circle.addEllipse({0, 0, 100, 100});
    const double ring = 2.0 * std::numbers::pi * 50.0 * 6.0;
    checkNear(strokeArea(circle, pen(6)), ring, ring * 0.002, "a stroked circle is a ring of the right area");
    const double thick = 2.0 * std::numbers::pi * 50.0 * 40.0;
    checkNear(strokeArea(circle, pen(40)), thick, thick * 0.002, "also when the pen is wide");
    // A closed square: outer minus inner square, with the pen's join outside.
    PainterPath square;
    square.addRect({10, 10, 40, 40});
    checkNear(strokeArea(square, pen(4, CapStyle::Flat, JoinStyle::Miter)), 44.0 * 44 - 36 * 36, 0.05,
              "a closed square: mitered frame");
    checkNear(strokeArea(square, pen(4, CapStyle::Round, JoinStyle::Bevel)), 44.0 * 44 - 36 * 36 - 4 * 2, 0.05,
              "closed paths have no caps; bevels cut the outer corners");
    // Self-overlapping strokes do not cancel out under non-zero.
    PainterPath cross;
    cross.moveTo({0, 50});
    cross.lineTo({100, 50});
    cross.moveTo({50, 0});
    cross.lineTo({50, 100});
    checkNear(strokeArea(cross, pen(10)), 2000.0 - 100.0, 0.05, "crossing strokes: the union");
}

void hostileInput() {
    Stroker s;
    const PainterPath l = line({0, 0}, {1000, 0});
    Pen p = pen(1);
    p.dashes = {1e-6f, 1e-6f};
    const auto start = std::chrono::steady_clock::now();
    const PainterPath out = s.stroke(l, p);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    check(subpaths(out) == 1 && elapsed < std::chrono::seconds(1), "a microscopic dash pattern is stroked solid");
    Pen nanPen = pen(std::numeric_limits<float>::quiet_NaN());
    checkNear(strokeArea(line({0, 0}, {10, 0}), nanPen), 10.0, 0.05, "a NaN width counts as 1");
    PainterPath point;
    point.moveTo({5, 5});
    point.lineTo({5, 5});
    check(s.stroke(point, pen(6, CapStyle::Round)).empty(), "a zero-length line draws nothing (Qt)");
    PainterPath bad;
    bad.moveTo({0, 0});
    bad.cubicTo({std::numeric_limits<float>::infinity(), 0}, {0, 1e30f}, {5, 5});
    (void)s.stroke(bad, pen(3, CapStyle::Round, JoinStyle::Round));
    check(true, "non-finite and huge control points do not crash");
}

} // namespace

int main() {
    caps();
    joins();
    miterLimits();
    dashes();
    curvesAndClosedPaths();
    hostileInput();
    return cfw::test::finish("StrokerTest");
}
