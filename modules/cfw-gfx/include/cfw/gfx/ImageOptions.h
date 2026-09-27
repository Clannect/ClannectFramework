#pragma once

// How Painter::drawImage samples and colours an image.
//
// Threads, allocation: a plain value type.

#include "cfw/core/Color.h"
#include "cfw/core/Vec2.h"

namespace cfw {

struct ImageOptions {
    // Bilinear filtering (Qt's SmoothPixmapTransform); false picks the
    // nearest pixel, for pixel art and exact blits.
    bool smooth = true;
    // Zero: the source rectangle is stretched over the target. Otherwise the
    // source repeats across the target, each copy `tileSize` units large,
    // starting at the target's top-left corner (QPainter::drawTiledPixmap).
    Vec2 tileSize{};
    // Multiplies every pixel, alpha included; white leaves the image as it is.
    // This is the image tint of the interface system in one step (it used to
    // be a Multiply pass and a DestinationIn pass).
    Color tint{1.0f, 1.0f, 1.0f, 1.0f};
};

} // namespace cfw
