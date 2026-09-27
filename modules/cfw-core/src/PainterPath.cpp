#include "cfw/core/PainterPath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace cfw {

namespace {

// Uniform subdivision error bounds: a chord of a curve with second
// derivative at most M, over a parameter step h, deviates at most M h^2 / 8.
constexpr std::size_t kMaxSegments = 1024; // per curve; bounds work on hostile input

std::size_t segmentsFor(float secondDerivativeBound, float tolerance) {
    if (!(secondDerivativeBound > 0.0f) || !std::isfinite(secondDerivativeBound)) {
        return 1;
    }
    const double n = std::ceil(std::sqrt(static_cast<double>(secondDerivativeBound) / (8.0 * tolerance)));
    return static_cast<std::size_t>(std::clamp(n, 1.0, static_cast<double>(kMaxSegments)));
}

float length(Vec2 v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y); }

bool finite(Vec2 p) noexcept { return std::isfinite(p.x) && std::isfinite(p.y); }

} // namespace

void PainterPath::moveTo(Vec2 p) {
    if (!finite(p)) {
        return; // as QPainterPath: invalid coordinates are ignored
    }
    if (!m_verbs.empty() && m_verbs.back() == Verb::Move) {
        m_points.back() = p; // consecutive moves: only the last one counts
    } else {
        m_verbs.push_back(Verb::Move);
        m_points.push_back(p);
    }
    m_subpathStart = m_points.size() - 1;
    m_open = true;
}

void PainterPath::ensureStarted() {
    if (!m_open) {
        // After close() the next element continues from the sub-path's start;
        // on an empty path, from the origin (as QPainterPath does).
        moveTo(m_points.empty() ? Vec2{} : m_points[m_subpathStart]);
    }
}

void PainterPath::lineTo(Vec2 p) {
    if (!finite(p)) {
        return;
    }
    ensureStarted();
    m_verbs.push_back(Verb::Line);
    m_points.push_back(p);
}

void PainterPath::quadTo(Vec2 control, Vec2 end) {
    if (!finite(control) || !finite(end)) {
        return;
    }
    ensureStarted();
    m_verbs.push_back(Verb::Quad);
    m_points.push_back(control);
    m_points.push_back(end);
}

void PainterPath::cubicTo(Vec2 control1, Vec2 control2, Vec2 end) {
    if (!finite(control1) || !finite(control2) || !finite(end)) {
        return;
    }
    ensureStarted();
    m_verbs.push_back(Verb::Cubic);
    m_points.push_back(control1);
    m_points.push_back(control2);
    m_points.push_back(end);
}

void PainterPath::close() {
    if (m_open && !m_verbs.empty() && m_verbs.back() != Verb::Move) {
        m_verbs.push_back(Verb::Close);
    }
    m_open = false;
}

Vec2 PainterPath::currentPoint() const noexcept {
    if (m_points.empty()) {
        return {};
    }
    return m_open ? m_points.back() : m_points[m_subpathStart];
}

void PainterPath::arcTo(const RectF &rect, float startDegrees, float sweepDegrees) {
    if (!std::isfinite(startDegrees) || !std::isfinite(sweepDegrees)) {
        return; // no arc to draw
    }
    sweepDegrees = std::clamp(sweepDegrees, -360.0f, 360.0f); // as Qt: more than a full turn is a full turn
    startDegrees = std::fmod(startDegrees, 360.0f);
    const double cx = rect.x + rect.width * 0.5;
    const double cy = rect.y + rect.height * 0.5;
    const double rx = rect.width * 0.5;
    const double ry = rect.height * 0.5;
    const auto pointAt = [&](double radians) {
        return Vec2{static_cast<float>(cx + rx * std::cos(radians)), static_cast<float>(cy - ry * std::sin(radians))};
    };
    const double start = startDegrees * std::numbers::pi / 180.0;
    const double sweep = sweepDegrees * std::numbers::pi / 180.0;
    const Vec2 first = pointAt(start);
    if (!m_open || m_points.empty() || currentPoint() != first) {
        lineTo(first);
    }
    // At most 90 degrees per cubic: the standard k = 4/3 tan(theta/4) fit.
    const int pieces = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / (std::numbers::pi / 2) - 1e-9)));
    const double step = sweep / pieces;
    const double k = 4.0 / 3.0 * std::tan(step / 4.0);
    for (int i = 0; i < pieces; ++i) {
        const double a0 = start + step * i;
        const double a1 = a0 + step;
        const Vec2 p0 = pointAt(a0);
        const Vec2 p1 = pointAt(a1);
        // The tangent of (cos a, -sin a) scaled by the radii.
        const Vec2 c1{static_cast<float>(p0.x - k * rx * std::sin(a0)), static_cast<float>(p0.y - k * ry * std::cos(a0))};
        const Vec2 c2{static_cast<float>(p1.x + k * rx * std::sin(a1)), static_cast<float>(p1.y + k * ry * std::cos(a1))};
        cubicTo(c1, c2, p1);
    }
}

void PainterPath::addRect(const RectF &rect) {
    moveTo({rect.x, rect.y});
    lineTo({rect.x + rect.width, rect.y});
    lineTo({rect.x + rect.width, rect.y + rect.height});
    lineTo({rect.x, rect.y + rect.height});
    close();
}

void PainterPath::addRoundedRect(const RectF &rect, float radiusX, float radiusY) {
    const float rx = std::min(std::abs(radiusX), std::abs(rect.width) * 0.5f);
    const float ry = std::min(std::abs(radiusY), std::abs(rect.height) * 0.5f);
    if (!(rx > 0.0f) || !(ry > 0.0f)) {
        addRect(rect);
        return;
    }
    const float x = rect.x;
    const float y = rect.y;
    const float w = rect.width;
    const float h = rect.height;
    moveTo({x + rx, y});
    // Clockwise on screen: negative sweeps.
    arcTo({x + w - 2 * rx, y, 2 * rx, 2 * ry}, 90, -90);
    arcTo({x + w - 2 * rx, y + h - 2 * ry, 2 * rx, 2 * ry}, 0, -90);
    arcTo({x, y + h - 2 * ry, 2 * rx, 2 * ry}, 270, -90);
    arcTo({x, y, 2 * rx, 2 * ry}, 180, -90);
    close();
}

void PainterPath::addEllipse(const RectF &rect) {
    moveTo({rect.x + rect.width, rect.y + rect.height * 0.5f});
    arcTo(rect, 0, -360); // clockwise from 3 o'clock, as Qt draws it
    close();
}

void PainterPath::addPolygon(Span<const Vec2> points, bool closed) {
    if (points.empty()) {
        return;
    }
    moveTo(points[0]);
    for (std::size_t i = 1; i < points.size(); ++i) {
        lineTo(points[i]);
    }
    if (closed) {
        close();
    }
}

void PainterPath::addPath(const PainterPath &other) {
    std::size_t p = 0;
    for (const Verb v : other.m_verbs) {
        switch (v) {
        case Verb::Move: moveTo(other.m_points[p++]); break;
        case Verb::Line: lineTo(other.m_points[p++]); break;
        case Verb::Quad:
            quadTo(other.m_points[p], other.m_points[p + 1]);
            p += 2;
            break;
        case Verb::Cubic:
            cubicTo(other.m_points[p], other.m_points[p + 1], other.m_points[p + 2]);
            p += 3;
            break;
        case Verb::Close: close(); break;
        }
    }
}

RectF PainterPath::controlBounds() const noexcept {
    if (m_points.empty()) {
        return {};
    }
    float x0 = std::numeric_limits<float>::infinity();
    float y0 = x0;
    float x1 = -x0;
    float y1 = -x0;
    for (const Vec2 p : m_points) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

PainterPath PainterPath::transformed(const Transform2D &transform) const {
    PainterPath out = *this;
    out.transform(transform);
    return out;
}

void PainterPath::transform(const Transform2D &transform) noexcept {
    for (Vec2 &p : m_points) {
        p = transform.map(p);
    }
}

void PainterPath::flatten(float tolerance, FlattenSink &sink) const {
    tolerance = std::max(tolerance, 1e-4f);
    std::size_t p = 0;
    Vec2 current{};
    Vec2 start{};
    bool open = false;
    const auto line = [&](Vec2 to) {
        if (!open) {
            sink.begin(current);
            start = current;
            open = true;
        }
        sink.point(to);
        current = to;
    };
    for (const Verb v : m_verbs) {
        switch (v) {
        case Verb::Move:
            if (open) {
                sink.end(false);
            }
            current = m_points[p++];
            start = current;
            sink.begin(current);
            open = true;
            break;
        case Verb::Line: line(m_points[p++]); break;
        case Verb::Quad: {
            const Vec2 p0 = current;
            const Vec2 c = m_points[p];
            const Vec2 e = m_points[p + 1];
            p += 2;
            // B'' = 2 (p0 - 2c + e)
            const float dd = 2.0f * length({p0.x - 2 * c.x + e.x, p0.y - 2 * c.y + e.y});
            const std::size_t n = segmentsFor(dd, tolerance);
            for (std::size_t i = 1; i <= n; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(n);
                const float u = 1.0f - t;
                line({u * u * p0.x + 2 * u * t * c.x + t * t * e.x, u * u * p0.y + 2 * u * t * c.y + t * t * e.y});
            }
            break;
        }
        case Verb::Cubic: {
            const Vec2 p0 = current;
            const Vec2 c1 = m_points[p];
            const Vec2 c2 = m_points[p + 1];
            const Vec2 e = m_points[p + 2];
            p += 3;
            // |B''| <= 6 max(|p0 - 2c1 + c2|, |c1 - 2c2 + e|)
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
                line({a * p0.x + b * c1.x + cc * c2.x + d * e.x, a * p0.y + b * c1.y + cc * c2.y + d * e.y});
            }
            break;
        }
        case Verb::Close:
            if (open) {
                sink.end(true);
                open = false;
            }
            current = start;
            break;
        }
    }
    if (open) {
        sink.end(false);
    }
}

std::vector<PainterPath::Polyline> PainterPath::flatten(float tolerance) const {
    struct Collect final : FlattenSink {
        std::vector<Polyline> out;
        void begin(Vec2 p) override { out.push_back({{p}, false}); }
        void point(Vec2 p) override { out.back().points.push_back(p); }
        void end(bool closed) override { out.back().closed = closed; }
    } collect;
    flatten(tolerance, collect);
    return std::move(collect.out);
}

void PainterPath::clear() noexcept {
    m_verbs.clear();
    m_points.clear();
    m_subpathStart = 0;
    m_open = false;
}

void PainterPath::reserve(std::size_t verbs, std::size_t points) {
    m_verbs.reserve(verbs);
    m_points.reserve(points);
}

} // namespace cfw
