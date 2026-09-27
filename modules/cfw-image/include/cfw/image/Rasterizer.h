#pragma once

// Anti-aliased scan conversion: turns filled paths into per-pixel coverage.
//
// Coverage is the exact area of each pixel inside the path, at 1/256-pixel
// precision, from signed area accumulation per cell (the technique of
// FreeType's "gray" rasteriser, which Qt's raster engine also derives from).
// Non-zero and even-odd fills are supported; where edges of different
// sub-paths cross inside one pixel the result is the usual approximation of
// this technique.
//
// Segments are clipped to the target first: parts above or below it are
// dropped, parts left of it become vertical edges at x = 0 (they still count
// for winding), parts right of it are dropped. So a hostile or huge path costs
// only its visible part, and non-finite coordinates are ignored.
//
// It lives in cfw-image because both cfw-text (glyph masks) and cfw-gfx
// (painting) use it; see docs/decisions/0014.
//
// Threads: one instance per thread. Allocates: the cell list, reused across
// reset() calls (steady-state rendering allocates nothing).

#include <cstdint>
#include <functional>
#include <vector>

#include "cfw/core/Path.h"
#include "cfw/core/Span.h"

namespace cfw {

class Rasterizer {
public:
    // Starts a new shape over a width x height target (pixels [0, width) x
    // [0, height)). Keeps the allocated storage.
    void reset(int width, int height);

    // Adds a path's outline. Curves are flattened to `tolerance` pixels in
    // target space (0.1 keeps a circle's area within 0.5%, closer than Qt's
    // raster engine): for an affine transform the path is transformed first,
    // for a perspective one it is flattened finely, then mapped.
    void addPath(const Path &path, const Transform2D &transform = {}, float tolerance = 0.1f);
    // A closed polygon in target coordinates.
    void addPolygon(Span<const Vec2> points);
    // One edge in target coordinates. Fills treat every added edge as part of
    // closed outlines; callers close their polygons.
    void addEdge(Vec2 from, Vec2 to);

    // Row callback: coverage for pixels [x, x + coverage.size()) of row y,
    // 0 (outside) to 255 (fully inside). Rows without coverage are skipped;
    // rows come in increasing y. The span is valid during the call only.
    using RowCallback = std::function<void(int y, int x, Span<const std::uint8_t> coverage)>;
    void sweep(FillRule rule, const RowCallback &row);

    // Bounding box of the covered pixels after the edges added so far
    // (empty if none).
    [[nodiscard]] bool empty() const noexcept { return m_cells.empty() && !m_haveCell; }

private:
    struct Cell {
        int x;
        int y;
        int cover;
        int area;
    };

    void lineFixed(std::int64_t x1, std::int64_t y1, std::int64_t x2, std::int64_t y2);
    void horizontalLine(std::int64_t ey, std::int64_t x1, std::int64_t y1, std::int64_t x2, std::int64_t y2);
    void setCell(std::int64_t x, std::int64_t y);
    void flushCell();
    void addClipped(double x0, double y0, double x1, double y1);

    int m_width = 0;
    int m_height = 0;
    std::vector<Cell> m_cells;
    Cell m_cell{0, 0, 0, 0};
    bool m_haveCell = false;
    std::vector<std::uint8_t> m_row;
    std::vector<std::uint32_t> m_rowStart;
    std::vector<Cell> m_sorted;
};

} // namespace cfw
