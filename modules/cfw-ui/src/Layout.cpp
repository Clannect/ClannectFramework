#include "cfw/ui/Layout.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "cfw/gfx/Painter.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Theme.h"

namespace cfw {

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

// One 1px line along an edge, inside `r`.
void edgeLine(Painter &painter, const RectF &r, Box::Side side, float width, Color color) {
    switch (side) {
    case Box::SideLeft: painter.fillRect({r.x, r.y, width, r.height}, color); break;
    case Box::SideTop: painter.fillRect({r.x, r.y, r.width, width}, color); break;
    case Box::SideRight: painter.fillRect({r.x + r.width - width, r.y, width, r.height}, color); break;
    case Box::SideBottom: painter.fillRect({r.x, r.y + r.height - width, r.width, width}, color); break;
    default: break;
    }
}

} // namespace

// ---- Box ----------------------------------------------------------------------------

Box::Box(Direction direction, float gap) : m_direction(direction), m_gap(gap) {}

void Box::setDirection(Direction direction) {
    m_direction = direction;
    invalidateLayout();
}
void Box::setGap(float gap) {
    m_gap = gap;
    invalidateLayout();
}
void Box::setLineGap(float gap) {
    m_lineGap = gap;
    invalidateLayout();
}
void Box::setPadding(Insets padding) {
    m_padding = padding;
    invalidateLayout();
}
void Box::setAlign(Align align) {
    m_align = align;
    invalidateLayout();
}
void Box::setJustify(Justify justify) {
    m_justify = justify;
    invalidateLayout();
}
void Box::setWrap(bool wrap) {
    m_wrap = wrap;
    invalidateLayout();
}
void Box::setMaxContentWidth(float width) {
    m_maxContentWidth = width;
    invalidateLayout();
}
void Box::setMinimumSize(Vec2 size) {
    m_minimum = size;
    invalidateLayout();
}
void Box::setBackground(std::optional<Color> color) {
    m_background = color;
    invalidatePaint();
}
void Box::setHoverBackground(std::optional<Color> color) {
    m_hoverBackground = color;
    invalidatePaint();
}
void Box::setPressedBackground(std::optional<Color> color) {
    m_pressedBackground = color;
    invalidatePaint();
}
void Box::setBorder(std::optional<Color> color, float width, std::uint8_t sides) {
    m_border = color;
    m_borderWidth = width;
    m_borderSides = sides;
    invalidatePaint();
}
void Box::setHoverBorder(std::optional<Color> color) {
    m_hoverBorder = color;
    invalidatePaint();
}
void Box::setRadius(float radius) {
    m_radius = radius;
    invalidatePaint();
}

void Box::setClickable(bool clickable) {
    m_clickable = clickable;
    setFocusable(clickable);
    setCursor(clickable ? std::optional<Cursor>(Cursor::Hand) : std::nullopt);
    if (clickable && role() == Role::None) {
        setRole(Role::Button);
    }
}

std::vector<Element *> Box::visibleChildren() const {
    std::vector<Element *> out;
    for (const auto &child : children()) {
        if (child->isVisible()) {
            out.push_back(child.get());
        }
    }
    return out;
}

RectF Box::contentRect(const RectF &rect) const {
    RectF inner{rect.x + m_padding.left, rect.y + m_padding.top, std::max(0.0f, rect.width - m_padding.width()),
                std::max(0.0f, rect.height - m_padding.height())};
    if (m_maxContentWidth > 0.0f && inner.width > m_maxContentWidth) {
        inner.x += std::floor((inner.width - m_maxContentWidth) / 2.0f);
        inner.width = m_maxContentWidth;
    }
    return inner;
}

void Box::resolveMain(float innerMain, float innerCross, std::vector<float> &mains) {
    const bool row = m_direction == Direction::Row;
    const std::vector<Element *> kids = visibleChildren();
    mains.assign(kids.size(), 0.0f);
    float natural = 0.0f;
    float weights = 0.0f;
    for (std::size_t i = 0; i < kids.size(); ++i) {
        const Vec2 available = row ? Vec2{innerMain, innerCross} : Vec2{innerCross, innerMain};
        const Vec2 size = kids[i]->measure(available);
        mains[i] = row ? size.x : size.y;
        natural += mains[i];
        weights += kids[i]->stretch();
    }
    const float gaps = kids.empty() ? 0.0f : m_gap * float(kids.size() - 1);
    if (!std::isfinite(innerMain) || weights <= 0.0f) {
        return;
    }
    const float free = innerMain - natural - gaps;
    if (free > 0.0f) {
        for (std::size_t i = 0; i < kids.size(); ++i) {
            mains[i] += free * kids[i]->stretch() / weights;
        }
        return;
    }
    // Too wide: the stretching children give up their width first, in
    // proportion, none below nothing (several rounds when some run out).
    float deficit = -free;
    for (int round = 0; round < 4 && deficit > 0.01f; ++round) {
        float w = 0.0f;
        for (std::size_t i = 0; i < kids.size(); ++i) {
            if (kids[i]->stretch() > 0.0f && mains[i] > 0.0f) {
                w += kids[i]->stretch();
            }
        }
        if (w <= 0.0f) {
            break;
        }
        float taken = 0.0f;
        for (std::size_t i = 0; i < kids.size(); ++i) {
            if (kids[i]->stretch() > 0.0f && mains[i] > 0.0f) {
                const float cut = std::min(mains[i], deficit * kids[i]->stretch() / w);
                mains[i] -= cut;
                taken += cut;
            }
        }
        deficit -= taken;
    }
}

Vec2 Box::measureWrapped(Vec2 inner, std::vector<Line> *lines) {
    const std::vector<Element *> kids = visibleChildren();
    const float lineGap = m_lineGap.value_or(m_gap);
    std::vector<Line> out;
    Line line;
    float widest = 0.0f;
    for (std::size_t i = 0; i < kids.size(); ++i) {
        const Vec2 size = kids[i]->measure(inner);
        const float needed = line.count == 0 ? size.x : line.main + m_gap + size.x;
        if (line.count > 0 && needed > inner.x) {
            out.push_back(line);
            line = Line{i, 0, 0.0f, 0.0f};
        }
        line.main = line.count == 0 ? size.x : line.main + m_gap + size.x;
        line.cross = std::max(line.cross, size.y);
        if (line.count == 0) {
            line.first = i;
        }
        ++line.count;
        widest = std::max(widest, line.main);
    }
    if (line.count > 0) {
        out.push_back(line);
    }
    float height = 0.0f;
    for (std::size_t i = 0; i < out.size(); ++i) {
        height += out[i].cross + (i > 0 ? lineGap : 0.0f);
    }
    if (lines) {
        *lines = std::move(out);
    }
    return {widest, height};
}

Vec2 Box::measureContent(Vec2 available) {
    Vec2 inner{std::max(0.0f, available.x - m_padding.width()), std::max(0.0f, available.y - m_padding.height())};
    if (m_maxContentWidth > 0.0f) {
        inner.x = std::min(inner.x, m_maxContentWidth);
    }
    Vec2 content;
    const bool row = m_direction == Direction::Row;
    if (row && m_wrap) {
        content = measureWrapped(inner, nullptr);
    } else {
        std::vector<float> mains;
        resolveMain(row ? inner.x : inner.y, row ? inner.y : inner.x, mains);
        const std::vector<Element *> kids = visibleChildren();
        float main = kids.empty() ? 0.0f : m_gap * float(kids.size() - 1);
        float cross = 0.0f;
        for (std::size_t i = 0; i < kids.size(); ++i) {
            main += mains[i];
            const Vec2 size = kids[i]->measure(row ? Vec2{mains[i], inner.y} : Vec2{inner.x, mains[i]});
            cross = std::max(cross, row ? size.y : size.x);
        }
        content = row ? Vec2{main, cross} : Vec2{cross, main};
    }
    const Vec2 size{content.x + m_padding.width(), content.y + m_padding.height()};
    return {std::max(size.x, m_minimum.x), std::max(size.y, m_minimum.y)};
}

void Box::arrangeContent(const RectF &rect) {
    const RectF inner = contentRect(rect);
    const std::vector<Element *> kids = visibleChildren();
    if (kids.empty()) {
        return;
    }
    const bool row = m_direction == Direction::Row;
    if (row && m_wrap) {
        std::vector<Line> lines;
        static_cast<void>(measureWrapped({inner.width, kInf}, &lines));
        const float lineGap = m_lineGap.value_or(m_gap);
        float y = inner.y;
        for (const Line &line : lines) {
            float x = inner.x;
            const float free = std::max(0.0f, inner.width - line.main);
            if (m_justify == Justify::Center) {
                x += std::floor(free / 2.0f);
            } else if (m_justify == Justify::End) {
                x += free;
            }
            for (std::size_t i = line.first; i < line.first + line.count; ++i) {
                const Vec2 size = kids[i]->measure({inner.width, kInf});
                float cy = y;
                float h = m_align == Align::Stretch ? line.cross : size.y;
                if (m_align == Align::Center) {
                    cy += std::floor((line.cross - h) / 2.0f);
                } else if (m_align == Align::End) {
                    cy += line.cross - h;
                }
                kids[i]->arrange({x, cy, size.x, h});
                x += size.x + m_gap;
            }
            y += line.cross + lineGap;
        }
        return;
    }
    const float innerMain = row ? inner.width : inner.height;
    const float innerCross = row ? inner.height : inner.width;
    std::vector<float> mains;
    resolveMain(innerMain, innerCross, mains);
    float used = m_gap * float(kids.size() - 1);
    for (const float m : mains) {
        used += m;
    }
    const float free = std::max(0.0f, innerMain - used);
    float pos = row ? inner.x : inner.y;
    float gap = m_gap;
    switch (m_justify) {
    case Justify::Start: break;
    case Justify::Center: pos += std::floor(free / 2.0f); break;
    case Justify::End: pos += free; break;
    case Justify::SpaceBetween:
        if (kids.size() > 1) {
            gap += free / float(kids.size() - 1);
        }
        break;
    }
    for (std::size_t i = 0; i < kids.size(); ++i) {
        const float main = mains[i];
        float cross = innerCross;
        if (m_align != Align::Stretch) {
            const Vec2 size = kids[i]->measure(row ? Vec2{main, innerCross} : Vec2{innerCross, main});
            cross = std::min(row ? size.y : size.x, innerCross);
        }
        float crossPos = row ? inner.y : inner.x;
        if (m_align == Align::Center) {
            crossPos += std::floor((innerCross - cross) / 2.0f);
        } else if (m_align == Align::End) {
            crossPos += innerCross - cross;
        }
        const float p = std::round(pos);
        const float extent = std::round(pos + main) - p;
        kids[i]->arrange(row ? RectF{p, crossPos, extent, cross} : RectF{crossPos, p, cross, extent});
        pos += main + gap;
    }
}

void Box::paint(Painter &painter, const Theme &) {
    const bool within = isHoverWithin();
    std::optional<Color> fill = m_background;
    if (m_pressedBackground && isPressed() && within) {
        fill = m_pressedBackground;
    } else if (m_hoverBackground && within) {
        fill = m_hoverBackground;
    }
    const RectF r = rect();
    if (fill && fill->a > 0.0f) {
        if (m_radius > 0.0f) {
            PainterPath shape;
            shape.addRoundedRect(r, m_radius, m_radius);
            painter.fillPath(shape, *fill);
        } else {
            painter.fillRect(r, *fill);
        }
    }
    const std::optional<Color> border = within && m_hoverBorder ? m_hoverBorder : m_border;
    if (border && m_borderWidth > 0.0f) {
        if (m_borderSides == SideAll) {
            const float h = m_borderWidth / 2.0f;
            PainterPath outline;
            outline.addRoundedRect(r.grownBy(-h, -h, -h, -h), std::max(0.0f, m_radius - h), std::max(0.0f, m_radius - h));
            painter.strokePath(outline, Pen(*border, m_borderWidth));
        } else {
            for (const Side side : {SideLeft, SideTop, SideRight, SideBottom}) {
                if (m_borderSides & side) {
                    edgeLine(painter, r, side, m_borderWidth, *border);
                }
            }
        }
    }
}

void Box::paintOverlay(Painter &painter, const Theme &theme) {
    const Surface *s = surface();
    if (!m_clickable || !hasFocus() || !s || !s->focusVisible()) {
        return;
    }
    const float w = theme.focusRingWidth;
    PainterPath ring;
    ring.addRoundedRect(rect().grownBy(-w / 2.0f, -w / 2.0f, -w / 2.0f, -w / 2.0f), m_radius, m_radius);
    painter.strokePath(ring, Pen(theme.accent, w));
}

bool Box::onPointer(const PointerEvent &event) {
    if (!m_clickable) {
        return false;
    }
    switch (event.type) {
    case PointerEvent::Type::Press:
        if (event.button == PointerButton::Right) {
            contextMenuRequested.emit(event.position);
            return true;
        }
        if (event.button != PointerButton::Left) {
            return false;
        }
        m_lastModifiers = event.modifiers;
        if (event.clickCount == 2) {
            doubleClicked.emit(); // last: a handler may replace this box
        }
        return true;
    case PointerEvent::Type::Release:
        if (event.button == PointerButton::Left && isPressed() && isHoverWithin()) {
            clicked.emit(); // last: a handler may replace this box
        }
        return true;
    default:
        return isPressed();
    }
}

bool Box::onKey(const KeyEvent &event) {
    if (m_clickable && event.type == KeyEvent::Type::Press && !event.repeat &&
        (event.key == Key::Space || event.key == Key::Enter)) {
        m_lastModifiers = event.modifiers;
        clicked.emit();
        return true;
    }
    return false;
}

void Box::onHoverWithinChanged(bool) {
    if (m_hoverBackground || m_hoverBorder) {
        invalidatePaint();
    }
}
void Box::onPressedChanged(bool) {
    if (m_pressedBackground) {
        invalidatePaint();
    }
}
void Box::onFocusChanged(bool) {
    if (m_clickable) {
        invalidatePaint();
    }
}

// ---- Grid ---------------------------------------------------------------------------

Grid::Grid(int columns, float gap) : m_columns(std::max(1, columns)), m_columnGap(gap), m_rowGap(gap) {}

void Grid::setColumns(int columns) {
    m_columns = std::max(1, columns);
    m_weights.clear();
    invalidateLayout();
}
void Grid::setColumnWeights(std::vector<float> weights) {
    m_weights = std::move(weights);
    invalidateLayout();
}
void Grid::setMinColumnWidth(float width) {
    m_minColumnWidth = width;
    invalidateLayout();
}
void Grid::setGap(float columnGap, float rowGap) {
    m_columnGap = columnGap;
    m_rowGap = rowGap;
    invalidateLayout();
}
void Grid::setPadding(Insets padding) {
    m_padding = padding;
    invalidateLayout();
}

std::size_t Grid::columnsFor(float width) const {
    if (m_minColumnWidth > 0.0f && std::isfinite(width)) {
        return std::max<std::size_t>(1, std::size_t((width + m_columnGap) / (m_minColumnWidth + m_columnGap)));
    }
    return m_weights.empty() ? std::size_t(m_columns) : m_weights.size();
}

std::vector<float> Grid::columnWidths(float width) const {
    const std::size_t n = columnsFor(width);
    const float usable = std::max(0.0f, width - m_columnGap * float(n - 1));
    std::vector<float> widths(n, usable / float(n));
    if (!m_weights.empty() && m_minColumnWidth <= 0.0f) {
        float total = 0.0f;
        for (const float w : m_weights) {
            total += std::max(0.0f, w);
        }
        for (std::size_t i = 0; i < n && total > 0.0f; ++i) {
            widths[i] = usable * std::max(0.0f, m_weights[i]) / total;
        }
    }
    return widths;
}

std::vector<Element *> Grid::visibleChildren() const {
    std::vector<Element *> out;
    for (const auto &child : children()) {
        if (child->isVisible()) {
            out.push_back(child.get());
        }
    }
    return out;
}

Vec2 Grid::measureContent(Vec2 available) {
    const std::vector<Element *> kids = visibleChildren();
    float width = std::max(0.0f, available.x - m_padding.width());
    if (!std::isfinite(width)) {
        // Unbounded: as wide as the widest child per column.
        float widest = 0.0f;
        for (Element *kid : kids) {
            widest = std::max(widest, kid->measure({kInf, kInf}).x);
        }
        const std::size_t n = columnsFor(kInf);
        width = widest * float(n) + m_columnGap * float(n - 1);
    }
    const std::vector<float> widths = columnWidths(width);
    const std::size_t n = widths.size();
    float height = 0.0f;
    for (std::size_t first = 0; first < kids.size(); first += n) {
        float rowHeight = 0.0f;
        for (std::size_t c = 0; c < n && first + c < kids.size(); ++c) {
            rowHeight = std::max(rowHeight, kids[first + c]->measure({widths[c], kInf}).y);
        }
        height += rowHeight + (first > 0 ? m_rowGap : 0.0f);
    }
    return {width + m_padding.width(), height + m_padding.height()};
}

void Grid::arrangeContent(const RectF &rect) {
    const std::vector<Element *> kids = visibleChildren();
    const RectF inner{rect.x + m_padding.left, rect.y + m_padding.top, std::max(0.0f, rect.width - m_padding.width()),
                      std::max(0.0f, rect.height - m_padding.height())};
    const std::vector<float> widths = columnWidths(inner.width);
    const std::size_t n = widths.size();
    float y = inner.y;
    for (std::size_t first = 0; first < kids.size(); first += n) {
        float rowHeight = 0.0f;
        for (std::size_t c = 0; c < n && first + c < kids.size(); ++c) {
            rowHeight = std::max(rowHeight, kids[first + c]->measure({widths[c], kInf}).y);
        }
        float x = inner.x;
        for (std::size_t c = 0; c < n && first + c < kids.size(); ++c) {
            const float px = std::round(x);
            kids[first + c]->arrange({px, y, std::round(x + widths[c]) - px, rowHeight});
            x += widths[c] + m_columnGap;
        }
        y += rowHeight + m_rowGap;
    }
}

} // namespace cfw
