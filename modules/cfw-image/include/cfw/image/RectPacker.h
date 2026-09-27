#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "cfw/core/Rect.h"

namespace cfw {

// Packs rectangles into a fixed-size atlas (glyphs, icons): skyline
// bottom-left, which keeps the free space as one staircase and places each
// rectangle as low, then as far left, as it fits. Good occupancy for many
// small rectangles of similar height, and O(skyline length) per insert.
//
// Positions have their origin at the atlas's top-left; "low" means small y.
// `padding` empty pixels are kept to the right of and below every
// rectangle, so bilinear sampling of one never reads its neighbour.
//
// Threads: one instance per thread. Allocates: the skyline (a few entries
// per placed rectangle, at most).
class RectPacker {
public:
    RectPacker(int width, int height, int padding = 0);

    // Where a width x height rectangle goes, or nothing if it no longer fits
    // (the atlas is unchanged then). Zero or negative sizes return nothing.
    [[nodiscard]] std::optional<Recti> insert(int width, int height);

    // Forgets every rectangle.
    void clear();

    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }
    // Fraction of the atlas covered by placed rectangles (padding excluded).
    [[nodiscard]] double occupancy() const noexcept;

private:
    struct Segment {
        int x;
        int y; // the skyline's height (first free row) over [x, x + width)
        int width;
    };

    int m_width;
    int m_height;
    int m_padding;
    std::vector<Segment> m_skyline;
    std::int64_t m_usedArea = 0;
};

} // namespace cfw
