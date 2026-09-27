#pragma once

// How painted pixels combine with what is already there. The formulas are
// Qt's composition modes on premultiplied colour (Porter-Duff and the
// separable blend modes); where a shape only partly covers a pixel, the
// result is mixed with the old pixel by the coverage, and pixels outside the
// shape are never touched (also as in Qt, unlike SVG compositing).
//
// Threads, allocation: a plain enum.

#include <cstdint>

namespace cfw {

enum class BlendMode : std::uint8_t {
    SourceOver,     // the default: source on top
    Source,         // replace
    DestinationIn,  // keep the destination where the source is opaque (masking)
    DestinationOut, // erase the destination where the source is opaque
    Multiply,       // darken: tinting
    Screen,         // lighten
    Plus,           // add, saturating
};

} // namespace cfw
