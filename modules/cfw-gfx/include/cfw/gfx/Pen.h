#pragma once

// How a path's outline is drawn: QPen's model, with Qt's defaults (1 unit
// wide, black, square caps, bevel joins, miter limit 2).
//
// As in Qt, dash lengths and the dash offset are in units of the pen width
// (a pattern {4, 2} on a 3-wide pen draws 12 on, 6 off), and a width of 0
// means a cosmetic 1-pixel pen.
//
// Threads: a value type. Allocates: only for more than eight dash entries.

#include <cstdint>

#include "cfw/core/SmallVector.h"
#include "cfw/gfx/Brush.h"

namespace cfw {

enum class CapStyle : std::uint8_t {
    Flat,   // ends exactly at the end point
    Square, // extends half the width past it (Qt's default)
    Round,  // a half circle past it
};

enum class JoinStyle : std::uint8_t {
    Bevel,    // corners cut straight (Qt's default)
    Miter,    // sharp; beyond the miter limit the point is cut off, as in Qt
    SvgMiter, // sharp; beyond the miter limit it becomes a bevel, as in SVG
    Round,
};

struct Pen {
    Brush brush{Color{0.0f, 0.0f, 0.0f, 1.0f}};
    float width = 1.0f;
    CapStyle cap = CapStyle::Square;
    JoinStyle join = JoinStyle::Bevel;
    // Miter: how far the point may reach, from where the outer edges would
    // otherwise stop, in pen widths. SvgMiter: SVG's ratio of miter length to
    // width.
    float miterLimit = 2.0f;
    // Alternating on and off lengths in pen widths; empty for a solid line.
    SmallVector<float, 8> dashes;
    float dashOffset = 0.0f;
    // A cosmetic pen's width is in device pixels: transforms do not scale it.
    bool cosmetic = false;

    Pen() = default;
    Pen(const Brush &b, float w) : brush(b), width(w) {}

    [[nodiscard]] bool isCosmetic() const noexcept { return cosmetic || width == 0.0f; }

    friend bool operator==(const Pen &a, const Pen &b) = default;
};

} // namespace cfw
