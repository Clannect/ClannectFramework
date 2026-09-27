#pragma once

// Turns a path's outline, drawn with a Pen, into a path whose non-zero fill
// is the stroke (what QPainterPathStroker::createStroke does). Every backend
// strokes this way, so strokes look the same on the CPU and the GPU.
//
// The geometry follows Qt's stroker, checked against QPainterPathStroker:
// - joins: outer corners get the pen's join; inner corners end where the
//   offset edges cross, or (hairpins, short segments) pass through the
//   corner point as in Qt, so overlaps add up under the non-zero rule;
// - Miter points reaching further than miterLimit * width past the outer
//   edges are cut off at that distance; SvgMiter falls back to a bevel;
// - dashes restart at each sub-path, run on around corners, and an odd-length
//   pattern drops its last entry; zero-length pieces (and zero-length
//   sub-paths) draw nothing, whatever the cap;
// - curves are flattened (finer for wider pens, so the outer edge still
//   stays within the tolerance) and joined smoothly inside themselves.
//
// The pen's width is used as given (0 means 1); cosmetic pens are the
// Painter's business (it strokes them in device space).
//
// Threads: one instance per thread. Allocates: scratch storage, reused
// across calls, and the output path's storage (reused if the same PainterPath is
// passed again).

#include <cstddef>
#include <vector>

#include "cfw/core/PainterPath.h"
#include "cfw/gfx/Pen.h"

namespace cfw {

class Stroker {
public:
    // Replaces `out` with the stroke outline of `path`. `tolerance` is the
    // largest allowed distance from the true outline, in path units.
    void stroke(const PainterPath &path, const Pen &pen, float tolerance, PainterPath &out);

    // Convenience: a fresh path.
    [[nodiscard]] PainterPath stroke(const PainterPath &path, const Pen &pen, float tolerance = 0.1f) {
        PainterPath out;
        stroke(path, pen, tolerance, out);
        return out;
    }

    // Bounds work on hostile patterns: a path needing more dash pieces than
    // this is stroked solid instead.
    static constexpr std::size_t kMaxDashPieces = 100000;

private:
    struct Point {
        Vec2 p;
        bool smooth; // inside a flattened curve: joined smoothly, not with the pen's join
    };
    struct Run {
        std::size_t begin;
        std::size_t count;
        bool closed;
    };

    void flatten(const PainterPath &path, float tolerance);
    void push(Vec2 p, bool smooth);
    void endRun(bool closed);
    bool dash(const Pen &pen, float width); // false: draw solid
    void strokeRun(const Point *points, const Run &run, PainterPath &out);
    void side(const Point *points, std::size_t count, bool closed, bool move, PainterPath &out);
    void join(Vec2 at, Vec2 d0, Vec2 d1, float shorter, bool smooth, PainterPath &out) const;
    void cap(Vec2 at, Vec2 d, PainterPath &out) const;
    static void arc(PainterPath &out, Vec2 center, float radius, Vec2 from, float sweep);

    std::vector<Point> m_points;
    std::vector<Run> m_runs;
    std::vector<Point> m_dashPoints;
    std::vector<Run> m_dashRuns;
    std::vector<Point> m_reversed;
    std::size_t m_runStart = 0;
    bool m_runInvalid = false;

    float m_half = 0.5f;
    CapStyle m_cap = CapStyle::Square;
    JoinStyle m_join = JoinStyle::Bevel;
    float m_miterLength = 2.0f; // Miter: miterLimit * width
    float m_miterLimit = 2.0f;
};

} // namespace cfw
