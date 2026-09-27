#include "cfw/image/RectPacker.h"

#include <algorithm>
#include <limits>

namespace cfw {

RectPacker::RectPacker(int width, int height, int padding)
    : m_width(std::max(width, 0)), m_height(std::max(height, 0)), m_padding(std::max(padding, 0)) {
    clear();
}

void RectPacker::clear() {
    m_skyline.clear();
    m_skyline.push_back({0, 0, m_width});
    m_usedArea = 0;
}

double RectPacker::occupancy() const noexcept {
    const auto total = static_cast<double>(m_width) * m_height;
    return total > 0 ? static_cast<double>(m_usedArea) / total : 0.0;
}

std::optional<Recti> RectPacker::insert(int width, int height) {
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    // Padding is kept to the right and below, except where it would run off
    // the atlas.
    const std::int64_t w = std::int64_t{width} + m_padding;
    const std::int64_t h = std::int64_t{height} + m_padding;
    std::size_t bestIndex = m_skyline.size();
    int bestX = 0;
    int bestY = std::numeric_limits<int>::max();
    for (std::size_t i = 0; i < m_skyline.size(); ++i) {
        const int x = m_skyline[i].x;
        if (x + std::int64_t{width} > m_width) {
            break;
        }
        // The highest segment under [x, x + w).
        int y = 0;
        std::int64_t covered = 0;
        for (std::size_t j = i; j < m_skyline.size() && covered < std::min<std::int64_t>(w, m_width - x); ++j) {
            y = std::max(y, m_skyline[j].y);
            covered += m_skyline[j].width;
        }
        if (y + std::int64_t{height} > m_height) {
            continue;
        }
        if (y < bestY || (y == bestY && x < bestX)) {
            bestIndex = i;
            bestX = x;
            bestY = y;
        }
    }
    if (bestIndex == m_skyline.size()) {
        return std::nullopt;
    }
    // Raise the skyline over [bestX, bestX + w), clipped to the atlas.
    const int newWidth = static_cast<int>(std::min<std::int64_t>(w, m_width - bestX));
    const int newTop = static_cast<int>(std::min<std::int64_t>(bestY + h, m_height));
    std::vector<Segment> next;
    next.reserve(m_skyline.size() + 2);
    const int end = bestX + newWidth;
    bool inserted = false;
    for (const Segment &s : m_skyline) {
        const int sEnd = s.x + s.width;
        if (sEnd <= bestX || s.x >= end) {
            if (!inserted && s.x >= end) {
                next.push_back({bestX, newTop, newWidth});
                inserted = true;
            }
            next.push_back(s);
            continue;
        }
        if (s.x < bestX) {
            next.push_back({s.x, s.y, bestX - s.x});
        }
        if (!inserted) {
            next.push_back({bestX, newTop, newWidth});
            inserted = true;
        }
        if (sEnd > end) {
            next.push_back({end, s.y, sEnd - end});
        }
    }
    if (!inserted) {
        next.push_back({bestX, newTop, newWidth});
    }
    // Merge neighbours of equal height.
    m_skyline.clear();
    for (const Segment &s : next) {
        if (!m_skyline.empty() && m_skyline.back().y == s.y) {
            m_skyline.back().width += s.width;
        } else {
            m_skyline.push_back(s);
        }
    }
    m_usedArea += std::int64_t{width} * height;
    return Recti{bestX, bestY, width, height};
}

} // namespace cfw
