#include "cfw/gfx/Stroker.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cfw {

namespace {

constexpr std::size_t kMaxCurveSegments = 1024;
constexpr float kPi = std::numbers::pi_v<float>;

// Chord error of uniform subdivision: M h^2 / 8 for second derivative M.
std::size_t segmentsFor(float secondDerivativeBound, float tolerance) {
    if (!(secondDerivativeBound > 0.0f) || !std::isfinite(secondDerivativeBound)) {
        return 1;
    }
    const double n = std::ceil(std::sqrt(static_cast<double>(secondDerivativeBound) / (8.0 * tolerance)));
    return static_cast<std::size_t>(std::clamp(n, 1.0, static_cast<double>(kMaxCurveSegments)));
}

float length(Vec2 v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y); }
Vec2 add(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
Vec2 sub(Vec2 a, Vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
Vec2 mul(Vec2 a, float k) noexcept { return {a.x * k, a.y * k}; }
float dot(Vec2 a, Vec2 b) noexcept { return a.x * b.x + a.y * b.y; }
float cross(Vec2 a, Vec2 b) noexcept { return a.x * b.y - a.y * b.x; }
Vec2 unit(Vec2 v) noexcept {
    const float l = length(v);
    return l > 0.0f ? Vec2{v.x / l, v.y / l} : Vec2{1.0f, 0.0f};
}
// The left-hand side on screen (y down): (1, 0) -> (0, -1).
Vec2 normal(Vec2 d) noexcept { return {d.y, -d.x}; }
Vec2 perp(Vec2 u) noexcept { return {-u.y, u.x}; }
Vec2 rotate(Vec2 v, float radians) noexcept {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}
bool same(Vec2 a, Vec2 b) noexcept { return std::abs(a.x - b.x) <= 1e-6f && std::abs(a.y - b.y) <= 1e-6f; }

} // namespace

void Stroker::stroke(const PainterPath &path, const Pen &pen, float tolerance, PainterPath &out) {
    out.clear();
    float width = std::abs(pen.width);
    if (!(width > 0.0f) || !std::isfinite(width)) {
        width = 1.0f;
    }
    m_half = width * 0.5f;
    m_cap = pen.cap;
    m_join = pen.join;
    m_miterLimit = std::isfinite(pen.miterLimit) ? std::max(pen.miterLimit, 0.0f) : 2.0f;
    m_miterLength = m_miterLimit * width;

    // The outer edge of a flattened curve sags (radius + half width) / radius
    // times the centre line's error: flatten finer for wide pens.
    tolerance = std::max(tolerance, 1e-4f);
    flatten(path, tolerance / (1.0f + m_half));
    // Dashing writes its pieces to separate buffers (never swapped, so every
    // buffer keeps its own steady-state capacity).
    const bool dashed = !pen.dashes.empty() && dash(pen, width);
    const std::vector<Point> &points = dashed ? m_dashPoints : m_points;
    for (const Run &run : dashed ? m_dashRuns : m_runs) {
        strokeRun(points.data() + run.begin, run, out);
    }
}

void Stroker::push(Vec2 p, bool smooth) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
        m_runInvalid = true; // e.g. a curve whose maths overflowed
        return;
    }
    if (m_points.size() > m_runStart && same(m_points.back().p, p)) {
        m_points.back().smooth = m_points.back().smooth && smooth;
        return;
    }
    m_points.push_back({p, smooth});
}

void Stroker::endRun(bool closed) {
    const std::size_t begin = m_runStart;
    if (m_runInvalid) {
        m_points.resize(begin); // a sub-path with non-finite points is not drawn
        m_runInvalid = false;
        m_runStart = m_points.size();
        return;
    }
    if (closed && m_points.size() - begin >= 2 && same(m_points.back().p, m_points[begin].p)) {
        m_points.pop_back();
    }
    const std::size_t count = m_points.size() - begin;
    if (count >= 2) {
        m_runs.push_back({begin, count, closed});
    } else {
        m_points.resize(begin); // a zero-length sub-path draws nothing, as in Qt
    }
    m_runStart = m_points.size();
}

void Stroker::flatten(const PainterPath &path, float tolerance) {
    m_points.clear();
    m_runs.clear();
    m_runStart = 0;
    m_runInvalid = false;
    const Span<const PainterPath::Verb> verbs = path.verbs();
    const Span<const Vec2> pts = path.points();
    std::size_t p = 0;
    Vec2 current{};
    Vec2 start{};
    bool open = false;
    const auto begin = [&](Vec2 at) {
        if (open) {
            endRun(false);
        }
        m_runStart = m_points.size();
        push(at, false);
        current = at;
        start = at;
        open = true;
    };
    for (const PainterPath::Verb v : verbs) {
        switch (v) {
        case PainterPath::Verb::Move: begin(pts[p++]); break;
        case PainterPath::Verb::Line:
            if (!open) {
                begin(current);
            }
            current = pts[p++];
            push(current, false);
            break;
        case PainterPath::Verb::Quad: {
            if (!open) {
                begin(current);
            }
            const Vec2 p0 = current;
            const Vec2 c = pts[p];
            const Vec2 e = pts[p + 1];
            p += 2;
            const std::size_t n = segmentsFor(2.0f * length({p0.x - 2 * c.x + e.x, p0.y - 2 * c.y + e.y}), tolerance);
            for (std::size_t i = 1; i <= n; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(n);
                const float u = 1.0f - t;
                push({u * u * p0.x + 2 * u * t * c.x + t * t * e.x, u * u * p0.y + 2 * u * t * c.y + t * t * e.y},
                     i < n);
            }
            current = e;
            break;
        }
        case PainterPath::Verb::Cubic: {
            if (!open) {
                begin(current);
            }
            const Vec2 p0 = current;
            const Vec2 c1 = pts[p];
            const Vec2 c2 = pts[p + 1];
            const Vec2 e = pts[p + 2];
            p += 3;
            const float m = 6.0f * std::max(length({p0.x - 2 * c1.x + c2.x, p0.y - 2 * c1.y + c2.y}),
                                            length({c1.x - 2 * c2.x + e.x, c1.y - 2 * c2.y + e.y}));
            const std::size_t n = segmentsFor(m, tolerance);
            for (std::size_t i = 1; i <= n; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(n);
                const float u = 1.0f - t;
                const float a = u * u * u;
                const float b = 3 * u * u * t;
                const float cc = 3 * u * t * t;
                const float d = t * t * t;
                push({a * p0.x + b * c1.x + cc * c2.x + d * e.x, a * p0.y + b * c1.y + cc * c2.y + d * e.y}, i < n);
            }
            current = e;
            break;
        }
        case PainterPath::Verb::Close:
            if (open) {
                endRun(true);
                open = false;
            }
            current = start;
            break;
        }
    }
    if (open) {
        endRun(false);
    }
}

bool Stroker::dash(const Pen &pen, float width) {
    // Qt: lengths in pen widths, negative entries count as zero, an odd
    // pattern drops its last entry.
    const std::size_t entries = pen.dashes.size() & ~std::size_t{1};
    float pattern[64];
    const std::size_t n = std::min<std::size_t>(entries, 64);
    float period = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const float v = pen.dashes[i];
        pattern[i] = std::isfinite(v) && v > 0.0f ? v * width : 0.0f;
        period += pattern[i];
    }
    if (n == 0 || !(period > 0.0f) || !std::isfinite(period)) {
        return false; // no usable pattern: solid
    }
    // Bound the work before doing it.
    double total = 0.0;
    for (const Run &run : m_runs) {
        for (std::size_t i = 0; i + 1 < run.count + (run.closed ? 1 : 0); ++i) {
            total += length(sub(m_points[run.begin + (i + 1) % run.count].p, m_points[run.begin + i].p));
        }
    }
    if (!(total / period * static_cast<double>(n) <= static_cast<double>(kMaxDashPieces))) {
        return false;
    }
    float offset = std::isfinite(pen.dashOffset) ? std::fmod(pen.dashOffset * width, period) : 0.0f;
    if (offset < 0.0f) {
        offset += period;
    }

    m_dashPoints.clear();
    m_dashRuns.clear();
    std::size_t pieceStart = 0;
    const auto startPiece = [&](Vec2 at) {
        pieceStart = m_dashPoints.size();
        m_dashPoints.push_back({at, false});
    };
    const auto addPoint = [&](Vec2 at, bool smooth) {
        if (same(m_dashPoints.back().p, at)) {
            m_dashPoints.back().smooth = m_dashPoints.back().smooth && smooth;
        } else {
            m_dashPoints.push_back({at, smooth});
        }
    };
    const auto endPiece = [&] {
        m_dashPoints.back().smooth = false;
        const std::size_t count = m_dashPoints.size() - pieceStart;
        if (count >= 2) {
            m_dashRuns.push_back({pieceStart, count, false});
        } else {
            m_dashPoints.resize(pieceStart); // zero length: nothing, as in Qt
        }
    };

    for (const Run &run : m_runs) {
        // The pattern restarts at every sub-path.
        std::size_t index = 0;
        float remaining = pattern[0];
        float skip = offset;
        while (skip > 0.0f) {
            if (skip >= remaining) {
                skip -= remaining;
                index = (index + 1) % n;
                remaining = pattern[index];
            } else {
                remaining -= skip;
                skip = 0.0f;
            }
        }
        bool on = index % 2 == 0;
        const Point &first = m_points[run.begin];
        if (on) {
            startPiece(first.p);
        }
        const std::size_t segments = run.count - (run.closed ? 0 : 1);
        for (std::size_t s = 0; s < segments; ++s) {
            const Vec2 a = m_points[run.begin + s].p;
            const Point &bp = m_points[run.begin + (s + 1) % run.count];
            const Vec2 d = sub(bp.p, a);
            const float len = length(d);
            const Vec2 dir = len > 0.0f ? mul(d, 1.0f / len) : Vec2{};
            float pos = 0.0f;
            while (len - pos > remaining) {
                pos += remaining;
                const Vec2 q = add(a, mul(dir, pos));
                if (on) {
                    addPoint(q, false);
                    endPiece();
                } else {
                    startPiece(q);
                }
                index = (index + 1) % n;
                remaining = pattern[index];
                on = index % 2 == 0;
            }
            remaining -= len - pos;
            if (on) {
                addPoint(bp.p, s + 1 < segments ? bp.smooth : false);
            }
        }
        if (on) {
            endPiece();
        }
    }
    return true;
}

void Stroker::strokeRun(const Point *pts, const Run &run, PainterPath &out) {
    const std::size_t n = run.count;
    m_reversed.assign(std::make_reverse_iterator(pts + n), std::make_reverse_iterator(pts));
    if (run.closed) {
        side(pts, n, true, true, out);
        out.close();
        side(m_reversed.data(), n, true, true, out);
        out.close();
        return;
    }
    side(pts, n, false, true, out);
    cap(pts[n - 1].p, unit(sub(pts[n - 1].p, pts[n - 2].p)), out);
    side(m_reversed.data(), n, false, false, out);
    cap(pts[0].p, unit(sub(pts[0].p, pts[1].p)), out);
    out.close();
}

void Stroker::side(const Point *pts, std::size_t n, bool closed, bool move, PainterPath &out) {
    const auto dir = [&](std::size_t i) { return unit(sub(pts[(i + 1) % n].p, pts[i].p)); };
    Vec2 d = dir(0);
    const Vec2 start = add(pts[0].p, mul(normal(d), m_half));
    if (move) {
        out.moveTo(start);
    } else {
        out.lineTo(start);
    }
    const std::size_t last = closed ? n : n - 1;
    const auto len = [&](std::size_t i) { return length(sub(pts[(i + 1) % n].p, pts[i].p)); };
    for (std::size_t i = 1; i < last; ++i) {
        const Vec2 next = dir(i);
        join(pts[i].p, d, next, std::min(len(i - 1), len(i)), pts[i].smooth, out);
        d = next;
    }
    if (closed) {
        const Vec2 first = dir(0);
        join(pts[0].p, d, first, std::min(len(n - 1), len(0)), pts[0].smooth, out);
    } else {
        out.lineTo(add(pts[n - 1].p, mul(normal(d), m_half)));
    }
}

// Emits the outline from p + h n(d0), the end of the incoming segment's edge,
// to p + h n(d1), the start of the outgoing one's (or, at an exact inner
// corner, just where the two edges cross). `shorter` is the length of the
// shorter segment.
void Stroker::join(Vec2 p, Vec2 d0, Vec2 d1, float shorter, bool smooth, PainterPath &out) const {
    const Vec2 n0 = normal(d0);
    const Vec2 n1 = normal(d1);
    const Vec2 a = add(p, mul(n0, m_half));
    const Vec2 b = add(p, mul(n1, m_half));
    const float c = cross(d0, d1);
    const float dt = dot(d0, d1);
    if (same(a, b)) {
        out.lineTo(a);
        return;
    }
    const float denom = 1.0f + dt;
    if (c < 0.0f || (c == 0.0f && dt > 0.0f)) {
        // Inner corner. Where the two offset edges cross within both segments
        // (half of each, so neighbouring corners cannot cross over), the
        // corner is that crossing: exact, with no overlap. Otherwise
        // (hairpins, segments shorter than the pen), pass through the corner
        // point as Qt always does, so the outlines overlap consistently
        // under the non-zero rule. Qt's way leaves overlap loops along every
        // curve, and area accumulation over-covers the pixels where they
        // meet an edge; this keeps stroked curves within 0.2% of their area.
        const float t = denom > 1e-6f ? m_half * std::abs(c) / denom : INFINITY;
        if (t <= shorter * 0.5f) {
            out.lineTo(sub(a, mul(d0, t)));
            return;
        }
        out.lineTo(a);
        out.lineTo(p);
        out.lineTo(b);
        return;
    }
    JoinStyle style = m_join;
    if (smooth) {
        style = dt > 0.985f ? JoinStyle::Miter : JoinStyle::Round; // inside a curve
    }
    out.lineTo(a);
    switch (style) {
    case JoinStyle::Bevel: out.lineTo(b); return;
    case JoinStyle::Miter: {
        // Distance from the outer edge's end to the miter point: h tan(turn / 2).
        const float t = denom > 1e-6f ? m_half * std::abs(c) / denom : INFINITY;
        if (smooth || t <= m_miterLength) {
            out.lineTo(add(a, mul(d0, t)));
        } else {
            out.lineTo(add(a, mul(d0, m_miterLength)));
            out.lineTo(sub(b, mul(d1, m_miterLength)));
        }
        out.lineTo(b);
        return;
    }
    case JoinStyle::SvgMiter: {
        // Miter length over width: 1 / cos(turn / 2).
        if (denom > 1e-6f && std::sqrt(2.0f / denom) <= m_miterLimit) {
            out.lineTo(add(a, mul(d0, m_half * std::abs(c) / denom)));
        }
        out.lineTo(b);
        return;
    }
    case JoinStyle::Round: {
        float sweep = std::atan2(cross(n0, n1), dot(n0, n1));
        if (dot(rotate(n0, sweep * 0.5f), d0) < 0.0f) {
            sweep -= std::copysign(2.0f * kPi, sweep);
        }
        arc(out, p, m_half, n0, sweep);
        return;
    }
    }
}

// At the current point p + h n(d); ends at p - h n(d).
void Stroker::cap(Vec2 p, Vec2 d, PainterPath &out) const {
    const Vec2 n = normal(d);
    switch (m_cap) {
    case CapStyle::Flat: break;
    case CapStyle::Square: {
        const Vec2 ext = mul(d, m_half);
        out.lineTo(add(add(p, mul(n, m_half)), ext));
        out.lineTo(add(sub(p, mul(n, m_half)), ext));
        break;
    }
    case CapStyle::Round: arc(out, p, m_half, n, kPi); break;
    }
}

void Stroker::arc(PainterPath &out, Vec2 center, float radius, Vec2 from, float sweep) {
    if (!std::isfinite(sweep) || !std::isfinite(radius) || std::abs(sweep) > 7.0f) {
        return; // never more than a half turn in practice; anything else is overflowed maths
    }
    const int pieces = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / (kPi / 2) - 1e-4f)));
    const float step = sweep / static_cast<float>(pieces);
    const float k = 4.0f / 3.0f * std::tan(step / 4.0f) * radius;
    for (int i = 0; i < pieces; ++i) {
        const Vec2 u0 = rotate(from, step * static_cast<float>(i));
        const Vec2 u1 = rotate(from, step * static_cast<float>(i + 1));
        const Vec2 p0 = add(center, mul(u0, radius));
        const Vec2 p1 = add(center, mul(u1, radius));
        out.cubicTo(add(p0, mul(perp(u0), k)), sub(p1, mul(perp(u1), k)), p1);
    }
}

} // namespace cfw
