#include "cfw/image/Rasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cfw {

namespace {

constexpr int kShift = 8;              // subpixel bits
constexpr std::int64_t kOne = 1 << kShift; // one pixel in subpixel units
constexpr std::int64_t kMask = kOne - 1;

// Floor division and modulo for possibly negative subpixel coordinates.
std::int64_t floorShift(std::int64_t v) noexcept { return v >> kShift; } // arithmetic shift floors

} // namespace

void Rasterizer::reset(int width, int height) {
    m_width = std::max(width, 0);
    m_height = std::max(height, 0);
    m_cells.clear();
    m_haveCell = false;
}

void Rasterizer::setCell(std::int64_t x, std::int64_t y) {
    // Everything left of the target shares one cell: only its cover matters.
    const int cx = static_cast<int>(std::clamp<std::int64_t>(x, -1, m_width));
    const int cy = static_cast<int>(y);
    if (m_haveCell && m_cell.x == cx && m_cell.y == cy) {
        return;
    }
    flushCell();
    m_cell = {cx, cy, 0, 0};
    m_haveCell = true;
}

void Rasterizer::flushCell() {
    if (m_haveCell && (m_cell.cover != 0 || m_cell.area != 0) && m_cell.x < m_width && m_cell.y >= 0 &&
        m_cell.y < m_height) {
        m_cells.push_back(m_cell);
    }
    m_haveCell = false;
}

// A line within one pixel row ey, from (x1, y1) to (x2, y2) where y1 and y2
// are subpixel offsets inside the row (0..kOne) and x are full subpixel
// coordinates. Adds cover (signed height) and area (height times twice the
// mean x offset in the cell) to each cell crossed.
void Rasterizer::horizontalLine(std::int64_t ey, std::int64_t x1, std::int64_t y1, std::int64_t x2, std::int64_t y2) {
    const std::int64_t ex1 = floorShift(x1);
    const std::int64_t ex2 = floorShift(x2);
    const std::int64_t fx1 = x1 & kMask;
    const std::int64_t fx2 = x2 & kMask;
    if (y1 == y2) {
        setCell(ex2, ey);
        return;
    }
    if (ex1 == ex2) {
        setCell(ex1, ey);
        m_cell.cover += static_cast<int>(y2 - y1);
        m_cell.area += static_cast<int>((fx1 + fx2) * (y2 - y1));
        return;
    }
    // Crosses several cells: step cell by cell, distributing dy.
    std::int64_t p = (kOne - fx1) * (y2 - y1);
    std::int64_t first = kOne;
    std::int64_t increment = 1;
    std::int64_t dx = x2 - x1;
    if (dx < 0) {
        p = fx1 * (y2 - y1);
        first = 0;
        increment = -1;
        dx = -dx;
    }
    std::int64_t delta = p / dx;
    std::int64_t mod = p % dx;
    if (mod < 0) {
        --delta;
        mod += dx;
    }
    std::int64_t ex = ex1;
    setCell(ex, ey);
    m_cell.area += static_cast<int>((fx1 + first) * delta);
    m_cell.cover += static_cast<int>(delta);
    ex += increment;
    std::int64_t y = y1 + delta;
    if (ex != ex2) {
        p = kOne * (y2 - y + delta);
        std::int64_t lift = p / dx;
        std::int64_t rem = p % dx;
        if (rem < 0) {
            --lift;
            rem += dx;
        }
        mod -= dx;
        while (ex != ex2) {
            delta = lift;
            mod += rem;
            if (mod >= 0) {
                mod -= dx;
                ++delta;
            }
            setCell(ex, ey);
            m_cell.area += static_cast<int>(kOne * delta);
            m_cell.cover += static_cast<int>(delta);
            y += delta;
            ex += increment;
        }
    }
    delta = y2 - y;
    setCell(ex2, ey);
    m_cell.area += static_cast<int>((fx2 + kOne - first) * delta);
    m_cell.cover += static_cast<int>(delta);
}

// A line in subpixel coordinates, split into pixel rows.
void Rasterizer::lineFixed(std::int64_t x1, std::int64_t y1, std::int64_t x2, std::int64_t y2) {
    std::int64_t dx = x2 - x1;
    std::int64_t dy = y2 - y1;
    const std::int64_t ey1 = floorShift(y1);
    const std::int64_t ey2 = floorShift(y2);
    const std::int64_t fy1 = y1 & kMask;
    const std::int64_t fy2 = y2 & kMask;
    if (ey1 == ey2) {
        horizontalLine(ey1, x1, fy1, x2, fy2);
        return;
    }
    std::int64_t increment = 1;
    if (dx == 0) {
        // Vertical: the same x offset in every row.
        const std::int64_t ex = floorShift(x1);
        const std::int64_t twoFx = (x1 - (ex << kShift)) << 1;
        std::int64_t first = kOne;
        if (dy < 0) {
            first = 0;
            increment = -1;
        }
        std::int64_t delta = first - fy1;
        setCell(ex, ey1);
        m_cell.cover += static_cast<int>(delta);
        m_cell.area += static_cast<int>(twoFx * delta);
        std::int64_t ey = ey1 + increment;
        delta = first + first - kOne;
        while (ey != ey2) {
            setCell(ex, ey);
            m_cell.cover += static_cast<int>(delta);
            m_cell.area += static_cast<int>(twoFx * delta);
            ey += increment;
        }
        delta = fy2 - kOne + first;
        setCell(ex, ey2);
        m_cell.cover += static_cast<int>(delta);
        m_cell.area += static_cast<int>(twoFx * delta);
        return;
    }
    // General case: the x where the line crosses each row boundary.
    std::int64_t p = (kOne - fy1) * dx;
    std::int64_t first = kOne;
    if (dy < 0) {
        p = fy1 * dx;
        first = 0;
        increment = -1;
        dy = -dy;
    }
    std::int64_t delta = p / dy;
    std::int64_t mod = p % dy;
    if (mod < 0) {
        --delta;
        mod += dy;
    }
    std::int64_t xFrom = x1 + delta;
    horizontalLine(ey1, x1, fy1, xFrom, first);
    std::int64_t ey = ey1 + increment;
    if (ey != ey2) {
        p = kOne * dx;
        std::int64_t lift = p / dy;
        std::int64_t rem = p % dy;
        if (rem < 0) {
            --lift;
            rem += dy;
        }
        mod -= dy;
        while (ey != ey2) {
            delta = lift;
            mod += rem;
            if (mod >= 0) {
                mod -= dy;
                ++delta;
            }
            const std::int64_t xTo = xFrom + delta;
            horizontalLine(ey, xFrom, kOne - first, xTo, first);
            xFrom = xTo;
            ey += increment;
        }
    }
    horizontalLine(ey, xFrom, kOne - first, x2, fy2);
}

// Clips to the target's rows, folds the part left of x = 0 onto x = 0 and
// drops the part right of the target, then rasterises.
void Rasterizer::addClipped(double x0, double y0, double x1, double y1) {
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) {
        return;
    }
    const double h = m_height;
    const double w = m_width;
    if ((y0 <= 0 && y1 <= 0) || (y0 >= h && y1 >= h) || y0 == y1) {
        return; // horizontal edges and edges outside the rows add no cover
    }
    // Clip to 0 <= y <= h.
    const auto atY = [&](double y) { return x0 + (x1 - x0) * (y - y0) / (y1 - y0); };
    if (y0 < 0) {
        x0 = atY(0);
        y0 = 0;
    } else if (y0 > h) {
        x0 = atY(h);
        y0 = h;
    }
    if (y1 < 0) {
        x1 = atY(0);
        y1 = 0;
    } else if (y1 > h) {
        x1 = atY(h);
        y1 = h;
    }
    // Split where the edge crosses x = 0 and x = w, in order along the edge.
    double ts[4] = {0, 0, 0, 1};
    int n = 1;
    for (const double bound : {0.0, w}) {
        if ((x0 < bound) != (x1 < bound) && x0 != x1) {
            const double t = (bound - x0) / (x1 - x0);
            if (t > 0 && t < 1) {
                ts[n++] = t;
            }
        }
    }
    ts[n++] = 1;
    std::sort(ts + 1, ts + n - 1);
    const auto fixed = [](double v) { return static_cast<std::int64_t>(std::llround(v * kOne)); };
    for (int i = 0; i + 1 < n; ++i) {
        const double ta = ts[i];
        const double tb = ts[i + 1];
        double ax = x0 + (x1 - x0) * ta;
        const double ay = y0 + (y1 - y0) * ta;
        double bx = x0 + (x1 - x0) * tb;
        const double by = y0 + (y1 - y0) * tb;
        const double mid = (ax + bx) * 0.5;
        if (mid >= w) {
            continue; // right of the target: affects no visible pixel
        }
        if (mid <= 0) {
            ax = bx = 0; // left of the target: keep its winding at x = 0
        }
        lineFixed(fixed(ax), fixed(ay), fixed(bx), fixed(by));
    }
}

void Rasterizer::addEdge(Vec2 from, Vec2 to) { addClipped(from.x, from.y, to.x, to.y); }

void Rasterizer::addPolygon(Span<const Vec2> points) {
    if (points.size() < 2) {
        return;
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Vec2 a = points[i];
        const Vec2 b = points[(i + 1) % points.size()];
        addClipped(a.x, a.y, b.x, b.y);
    }
}

void Rasterizer::addPath(const Path &path, const Transform2D &transform, float tolerance) {
    if (transform.isAffine()) {
        const Path mapped = transform.isIdentity() ? Path() : path.transformed(transform);
        for (const Path::Polyline &poly : (transform.isIdentity() ? path : mapped).flatten(tolerance)) {
            addPolygon(poly.points);
        }
        return;
    }
    // Perspective: flatten in source space finely enough that the mapped
    // chords stay within tolerance for moderate foreshortening, then map.
    for (Path::Polyline &poly : path.flatten(tolerance * 0.25f)) {
        for (Vec2 &p : poly.points) {
            p = transform.map(p);
        }
        addPolygon(poly.points);
    }
}

void Rasterizer::sweep(FillRule rule, const RowCallback &row) {
    flushCell();
    if (m_cells.empty() || m_width == 0) {
        return;
    }
    // Bucket cells by row (counting sort), then sort each row by x.
    m_rowStart.assign(static_cast<std::size_t>(m_height) + 1, 0);
    for (const Cell &c : m_cells) {
        ++m_rowStart[static_cast<std::size_t>(c.y) + 1];
    }
    for (std::size_t y = 1; y < m_rowStart.size(); ++y) {
        m_rowStart[y] += m_rowStart[y - 1];
    }
    m_sorted.resize(m_cells.size());
    {
        std::vector<std::uint32_t> &next = m_rowStart; // reuse as insertion cursors, restored below
        for (const Cell &c : m_cells) {
            m_sorted[next[static_cast<std::size_t>(c.y)]++] = c;
        }
        for (std::size_t y = m_rowStart.size() - 1; y > 0; --y) {
            m_rowStart[y] = m_rowStart[y - 1];
        }
        m_rowStart[0] = 0;
    }
    m_row.resize(static_cast<std::size_t>(m_width));
    const bool evenOdd = rule == FillRule::EvenOdd;
    // Accumulated area (in units of 2 * kOne * kOne per pixel) to 0..255.
    const auto alpha = [evenOdd](std::int64_t area) -> std::uint8_t {
        std::int64_t cover = area >> (2 * kShift + 1 - 8);
        if (cover < 0) {
            cover = -cover;
        }
        if (evenOdd) {
            cover &= 511;
            if (cover > 256) {
                cover = 512 - cover;
            }
        }
        return static_cast<std::uint8_t>(std::min<std::int64_t>(cover, 255));
    };
    for (std::size_t y = 0; y + 1 < m_rowStart.size(); ++y) {
        const std::uint32_t begin = m_rowStart[y];
        const std::uint32_t end = m_rowStart[y + 1];
        if (begin == end) {
            continue;
        }
        std::sort(m_sorted.begin() + begin, m_sorted.begin() + end, [](const Cell &a, const Cell &b) { return a.x < b.x; });
        std::int64_t cover = 0; // winding accumulated from the left, in subpixel rows
        int startX = -1;
        int endX = 0;
        const auto fill = [&](int from, int to) {
            if (to > from) {
                std::memset(m_row.data() + from, alpha(cover << (kShift + 1)), static_cast<std::size_t>(to - from));
            }
        };
        std::uint32_t i = begin;
        while (i < end) {
            const int x = m_sorted[i].x;
            if (x < 0) {
                cover += m_sorted[i].cover; // left of the target: only its winding matters
                ++i;
                continue;
            }
            // Pixels since the last cell are uniformly covered by the winding so far.
            if (startX < 0) {
                startX = cover != 0 ? 0 : x;
                endX = startX;
            }
            fill(endX, x);
            std::int64_t area = 0;
            while (i < end && m_sorted[i].x == x) {
                area += m_sorted[i].area;
                cover += m_sorted[i].cover;
                ++i;
            }
            m_row[static_cast<std::size_t>(x)] = alpha((cover << (kShift + 1)) - area);
            endX = x + 1;
        }
        if (cover != 0) {
            // Still inside at the last cell: covered to the target's right edge.
            if (startX < 0) {
                startX = 0;
                endX = 0;
            }
            fill(endX, m_width);
            endX = m_width;
        }
        if (startX >= 0 && endX > startX) {
            row(static_cast<int>(y), startX,
                Span<const std::uint8_t>(m_row.data() + startX, static_cast<std::size_t>(endX - startX)));
        }
    }
    m_cells.clear();
}

} // namespace cfw
