#include "cfw/gfx/Brush.h"

#include <algorithm>
#include <cmath>

namespace cfw {

Brush Brush::linearGradient(Vec2 start, Vec2 end, Span<const GradientStop> stops, GradientSpread spread) {
    Brush b;
    b.m_kind = Kind::LinearGradient;
    b.m_spread = spread;
    b.m_a = start;
    b.m_b = end;
    b.setStops(stops);
    return b;
}

Brush Brush::radialGradient(Vec2 center, float radius, Vec2 focal, Span<const GradientStop> stops,
                            GradientSpread spread) {
    Brush b;
    b.m_kind = Kind::RadialGradient;
    b.m_spread = spread;
    b.m_a = center;
    b.m_radius = std::abs(radius);
    // Keep the focal point strictly inside the circle so every pixel has one
    // gradient offset.
    const float dx = focal.x - center.x;
    const float dy = focal.y - center.y;
    const float distance = std::sqrt(dx * dx + dy * dy);
    const float limit = b.m_radius * 0.999f;
    if (distance > limit) {
        const float k = distance > 0.0f ? limit / distance : 0.0f;
        focal = {center.x + dx * k, center.y + dy * k};
    }
    b.m_b = focal;
    b.setStops(stops);
    return b;
}

// GCC 13 at -O3 inlines SmallVector's growth path here and reports a
// bogus -Warray-bounds on the inline storage (the element count is only known
// at run time); the code is checked under ASan and UBSan.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
void Brush::setStops(Span<const GradientStop> stops) {
    m_stops.clear();
    for (const GradientStop &s : stops) {
        if (std::isfinite(s.offset)) {
            m_stops.push_back({std::clamp(s.offset, 0.0f, 1.0f), s.color});
        }
    }
    if (m_stops.empty()) {
        m_stops.push_back({0.0f, Color{0.0f, 0.0f, 0.0f, 1.0f}});
        m_stops.push_back({1.0f, Color{1.0f, 1.0f, 1.0f, 1.0f}});
    }
    // Stable insertion sort: few stops, and equal offsets keep their order
    // (a hard edge).
    for (std::size_t i = 1; i < m_stops.size(); ++i) {
        const GradientStop stop = m_stops[i];
        std::size_t j = i;
        for (; j > 0 && m_stops[j - 1].offset > stop.offset; --j) {
            m_stops[j] = m_stops[j - 1];
        }
        m_stops[j] = stop;
    }
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

bool Brush::isOpaque() const noexcept {
    switch (m_kind) {
    case Kind::None: return false;
    case Kind::Solid: return m_color.a >= 1.0f;
    case Kind::LinearGradient:
    case Kind::RadialGradient:
        return std::all_of(m_stops.begin(), m_stops.end(), [](const GradientStop &s) { return s.color.a >= 1.0f; });
    }
    return false;
}

} // namespace cfw
