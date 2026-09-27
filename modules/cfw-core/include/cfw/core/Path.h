#pragma once

// A vector path: sub-paths of lines and quadratic and cubic Béziers, the
// geometry that fills, strokes, clips and glyph outlines are made of. It
// lives in cfw-core (like Transform2D) because both cfw-text (glyphs) and
// cfw-gfx (drawing) build on it; see docs/decisions/0014.
//
// Coordinates are y-down, like the screen. Angles are in degrees and, as in
// Qt's QPainterPath, positive angles turn counter-clockwise *on screen*
// (towards negative y).
//
// Threads: a value type. Allocates: the element and point storage.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cfw/core/Rect.h"
#include "cfw/core/Span.h"
#include "cfw/core/Transform2D.h"
#include "cfw/core/Vec2.h"

namespace cfw {

// Which points a path covers when sub-paths overlap or wind.
enum class FillRule : std::uint8_t {
    NonZero, // inside where the winding number is not zero (Qt::WindingFill)
    EvenOdd, // inside where it is odd (Qt::OddEvenFill, QPainterPath's default)
};

class Path {
public:
    enum class Verb : std::uint8_t { Move, Line, Quad, Cubic, Close };

    Path() = default;

    // Starts a new sub-path at `p`.
    void moveTo(Vec2 p);
    // Each drawing call starts a sub-path at (0, 0) if none is open.
    void lineTo(Vec2 p);
    void quadTo(Vec2 control, Vec2 end);
    void cubicTo(Vec2 control1, Vec2 control2, Vec2 end);
    // Closes the current sub-path with a straight line back to its start.
    void close();

    // Qt's arcTo: an arc of the ellipse inscribed in `rect`, starting at
    // `startDegrees` and sweeping `sweepDegrees` (positive = counter-clockwise
    // on screen). A line joins the current point to the arc's start.
    void arcTo(const RectF &rect, float startDegrees, float sweepDegrees);

    // Closed shapes, each a new sub-path, drawn clockwise on screen.
    void addRect(const RectF &rect);
    // Corner radii are clamped to half the rectangle's size.
    void addRoundedRect(const RectF &rect, float radiusX, float radiusY);
    void addEllipse(const RectF &rect);
    void addPolygon(Span<const Vec2> points, bool closed = true);
    void addPath(const Path &other);

    [[nodiscard]] bool empty() const noexcept { return m_verbs.empty(); }
    [[nodiscard]] Span<const Verb> verbs() const noexcept { return m_verbs; }
    [[nodiscard]] Span<const Vec2> points() const noexcept { return m_points; }
    // The current point (the end of the last element), or (0, 0).
    [[nodiscard]] Vec2 currentPoint() const noexcept;

    // Bounds of every point, control points included (a cheap superset of
    // the curve's bounds). Empty for an empty path.
    [[nodiscard]] RectF controlBounds() const noexcept;

    // A copy with every point mapped. Exact for affine transforms; for a
    // perspective transform, flatten first (curves do not stay curves).
    [[nodiscard]] Path transformed(const Transform2D &transform) const;

    // The path as polylines, curves subdivided until no point is further than
    // `tolerance` from the true curve. Each polyline is one sub-path; `closed`
    // tells whether it was closed (fills treat every sub-path as closed).
    struct Polyline {
        std::vector<Vec2> points;
        bool closed = false;
    };
    [[nodiscard]] std::vector<Polyline> flatten(float tolerance = 0.25f) const;

    void clear() noexcept;
    void reserve(std::size_t verbs, std::size_t points);

    friend bool operator==(const Path &a, const Path &b) = default;

private:
    void ensureStarted();

    std::vector<Verb> m_verbs;
    std::vector<Vec2> m_points;
    std::size_t m_subpathStart = 0; // index into m_points of the open sub-path's start
    bool m_open = false;
};

} // namespace cfw
