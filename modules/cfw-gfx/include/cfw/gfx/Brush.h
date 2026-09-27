#pragma once

// What a fill or stroke paints with: nothing, a solid colour, or a linear or
// radial gradient (QBrush's subset that Clannect uses).
//
// Gradient geometry is in the coordinates of the shape being painted, so it
// moves with the painter's transform, as in Qt. Colours between stops are
// interpolated in premultiplied sRGB (QGradient::ColorInterpolation, Qt's
// default): a stop fading to transparent does not darken on the way.
//
// Threads: a value type. Allocates: only for more than four gradient stops.

#include <cstdint>

#include "cfw/core/Color.h"
#include "cfw/core/SmallVector.h"
#include "cfw/core/Span.h"
#include "cfw/core/Vec2.h"

namespace cfw {

// What a gradient does beyond its ends.
enum class GradientSpread : std::uint8_t {
    Pad,     // the end colours continue (Qt's default)
    Repeat,  // the gradient starts over
    Reflect, // the gradient runs back and forth
};

struct GradientStop {
    float offset = 0.0f; // 0 to 1
    Color color;

    friend constexpr bool operator==(const GradientStop &a, const GradientStop &b) noexcept = default;
};

class Brush {
public:
    enum class Kind : std::uint8_t { None, Solid, LinearGradient, RadialGradient };

    // Paints nothing (Qt::NoBrush).
    Brush() noexcept = default;
    // A solid colour; implicit, as QBrush(QColor) is.
    Brush(const Color &color) noexcept : m_kind(Kind::Solid), m_color(color) {} // NOLINT(google-explicit-constructor)

    // Colour runs from `start` (offset 0) to `end` (offset 1) along the line
    // between them, constant across it.
    [[nodiscard]] static Brush linearGradient(Vec2 start, Vec2 end, Span<const GradientStop> stops,
                                              GradientSpread spread = GradientSpread::Pad);
    // Offset 0 at `focal`, offset 1 on the circle (center, radius). A focal
    // point on or outside the circle is moved just inside it, as Qt does.
    [[nodiscard]] static Brush radialGradient(Vec2 center, float radius, Vec2 focal, Span<const GradientStop> stops,
                                              GradientSpread spread = GradientSpread::Pad);
    [[nodiscard]] static Brush radialGradient(Vec2 center, float radius, Span<const GradientStop> stops,
                                              GradientSpread spread = GradientSpread::Pad) {
        return radialGradient(center, radius, center, stops, spread);
    }

    [[nodiscard]] Kind kind() const noexcept { return m_kind; }
    [[nodiscard]] bool isNone() const noexcept { return m_kind == Kind::None; }
    // Solid brushes: the colour.
    [[nodiscard]] Color color() const noexcept { return m_color; }
    // Gradients: stops sorted by offset (stable, so equal offsets make a hard
    // edge), offsets clamped to [0, 1]. With no stops given, black to white.
    [[nodiscard]] Span<const GradientStop> stops() const noexcept { return m_stops; }
    [[nodiscard]] GradientSpread spread() const noexcept { return m_spread; }
    // Linear: start and end. Radial: centre and focal point.
    [[nodiscard]] Vec2 start() const noexcept { return m_a; }
    [[nodiscard]] Vec2 end() const noexcept { return m_b; }
    [[nodiscard]] Vec2 center() const noexcept { return m_a; }
    [[nodiscard]] Vec2 focal() const noexcept { return m_b; }
    [[nodiscard]] float radius() const noexcept { return m_radius; }

    // True if everything this brush paints is fully opaque.
    [[nodiscard]] bool isOpaque() const noexcept;

    friend bool operator==(const Brush &a, const Brush &b) = default;

private:
    void setStops(Span<const GradientStop> stops);

    Kind m_kind = Kind::None;
    GradientSpread m_spread = GradientSpread::Pad;
    Color m_color{0.0f, 0.0f, 0.0f, 1.0f};
    Vec2 m_a{};
    Vec2 m_b{};
    float m_radius = 0.0f;
    SmallVector<GradientStop, 4> m_stops;
};

} // namespace cfw
