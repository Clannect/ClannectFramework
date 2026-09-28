#include "cfw/ui/Views.h"

#include <algorithm>
#include <cmath>

#include "ControlText.h"
#include "cfw/gfx/Painter.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Theme.h"

namespace cfw {

using detail::centredOrigin;
using detail::layoutIn;

namespace {
const float kInfinity = std::numeric_limits<float>::infinity();
constexpr float kIndent = 16.0f;

bool ctrlHeld(Modifier m) { return hasModifier(m, Modifier::Control) || hasModifier(m, Modifier::Meta); }

void triangle(Painter &painter, Vec2 c, bool open, const Color &color) {
    PainterPath path;
    if (open) {
        path.moveTo({c.x - 4, c.y - 2});
        path.lineTo({c.x + 4, c.y - 2});
        path.lineTo({c.x, c.y + 3});
    } else {
        path.moveTo({c.x - 2, c.y - 4});
        path.lineTo({c.x + 3, c.y});
        path.lineTo({c.x - 2, c.y + 4});
    }
    path.close();
    painter.fillPath(path, color);
}
} // namespace

// ---- Splitter ---------------------------------------------------------------------

Splitter::Splitter(Stack::Direction direction, float ratio) : m_direction(direction), m_ratio(std::clamp(ratio, 0.0f, 1.0f)) {
    setRole(Role::Group);
}

std::optional<Cursor> Splitter::cursorAt(Vec2 position) const {
    const RectF h = handle();
    if (m_dragging || (position.x >= h.x && position.y >= h.y && position.x < h.right() && position.y < h.bottom())) {
        return m_direction == Stack::Direction::Row ? Cursor::SizeHorizontal : Cursor::SizeVertical;
    }
    return Element::cursorAt(position);
}

Element *Splitter::pane(std::size_t index) const { return index == 0 ? m_first : m_second; }

Element &Splitter::setFirst(std::unique_ptr<Element> pane) {
    if (m_first) {
        static_cast<void>(remove(*m_first));
    }
    m_first = &add(std::move(pane));
    return *m_first;
}

Element &Splitter::setSecond(std::unique_ptr<Element> pane) {
    if (m_second) {
        static_cast<void>(remove(*m_second));
    }
    m_second = &add(std::move(pane));
    return *m_second;
}

void Splitter::setRatio(float ratio) {
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    if (ratio == m_ratio) {
        return;
    }
    m_ratio = ratio;
    arrangeContent(rect());
    invalidatePaint();
    ratioChanged.emit(m_ratio);
}

RectF Splitter::handle() const {
    const RectF r = rect();
    const bool row = m_direction == Stack::Direction::Row;
    const float length = (row ? r.width : r.height) - kHandle;
    float first = std::round(length * m_ratio);
    first = std::clamp(first, std::min(m_minimum, length / 2), std::max(length - m_minimum, length / 2));
    return row ? RectF{r.x + first, r.y, kHandle, r.height} : RectF{r.x, r.y + first, r.width, kHandle};
}

void Splitter::arrangeContent(const RectF &r) {
    const RectF h = handle();
    if (m_direction == Stack::Direction::Row) {
        if (m_first) m_first->arrange({r.x, r.y, h.x - r.x, r.height});
        if (m_second) m_second->arrange({h.right(), r.y, r.right() - h.right(), r.height});
    } else {
        if (m_first) m_first->arrange({r.x, r.y, r.width, h.y - r.y});
        if (m_second) m_second->arrange({r.x, h.bottom(), r.width, r.bottom() - h.bottom()});
    }
}

void Splitter::paint(Painter &painter, const Theme &theme) {
    painter.fillRect(handle(), m_dragging || isHovered() ? theme.accent : theme.border);
}

bool Splitter::onPointer(const PointerEvent &event) {
    const RectF h = handle().grownBy(2, 2, 2, 2);
    const bool onHandle = event.position.x >= h.x && event.position.x < h.right() && event.position.y >= h.y &&
                          event.position.y < h.bottom();
    switch (event.type) {
    case PointerEvent::Type::Press:
        m_dragging = onHandle && event.button == PointerButton::Left;
        return m_dragging;
    case PointerEvent::Type::Move:
        if (m_dragging) {
            const RectF r = rect();
            const bool row = m_direction == Stack::Direction::Row;
            const float length = std::max(1.0f, (row ? r.width : r.height) - kHandle);
            const float at = (row ? event.position.x - r.x : event.position.y - r.y) - kHandle / 2;
            setRatio(at / length);
            return true;
        }
        return false;
    case PointerEvent::Type::Release:
        if (m_dragging) {
            m_dragging = false;
            invalidatePaint();
            return true;
        }
        return false;
    default:
        return false;
    }
}

// ---- TabBar -----------------------------------------------------------------------

TabBar::TabBar(std::vector<String> tabs) : m_tabs(std::move(tabs)) {
    setRole(Role::Tab);
    setFocusable(true);
}

void TabBar::setTabs(std::vector<String> tabs) {
    m_tabs = std::move(tabs);
    m_current = std::clamp(m_current, 0, std::max(0, int(m_tabs.size()) - 1));
    invalidateLayout();
    invalidatePaint();
}

void TabBar::setCurrentIndex(int index) {
    m_current = std::clamp(index, 0, std::max(0, int(m_tabs.size()) - 1));
    invalidatePaint();
}

void TabBar::choose(int index) {
    if (index < 0 || index >= int(m_tabs.size()) || index == m_current) {
        return;
    }
    setCurrentIndex(index);
    currentChanged.emit(m_current);
}

std::vector<float> TabBar::widths() const {
    const Theme &t = theme();
    std::vector<float> out;
    TextLayout layout;
    for (const String &tab : m_tabs) {
        const float text = layoutIn(layout, tab, t, kInfinity, false, TextAlign::Start) ? std::ceil(layout.size().x) : 40.0f;
        out.push_back(text + 2 * t.padding * 1.5f);
    }
    return out;
}

Vec2 TabBar::measureContent(Vec2) {
    float total = 0.0f;
    for (const float w : widths()) {
        total += w;
    }
    return {total, theme().controlHeight + 4.0f};
}

int TabBar::tabAt(float x) const {
    float left = rect().x;
    const std::vector<float> w = widths();
    for (std::size_t i = 0; i < w.size(); ++i) {
        if (x >= left && x < left + w[i]) {
            return int(i);
        }
        left += w[i];
    }
    return -1;
}

void TabBar::paint(Painter &painter, const Theme &theme) {
    const RectF r = rect();
    painter.fillRect(r, theme.panel);
    painter.fillRect({r.x, r.bottom() - 1, r.width, 1}, theme.border);
    const std::vector<float> w = widths();
    float left = r.x;
    TextLayout layout;
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        const RectF tab{left, r.y, w[i], r.height};
        const bool current = int(i) == m_current;
        if (current) {
            painter.fillRect(tab, theme.window);
            painter.fillRect({tab.x, tab.bottom() - 2, tab.width, 2}, theme.accent);
        } else if (int(i) == m_hoveredTab) {
            painter.fillRect(tab, theme.controlHover);
        }
        if (layoutIn(layout, m_tabs[i], theme, kInfinity, false, TextAlign::Start)) {
            const Vec2 origin = centredOrigin(layout, tab);
            painter.drawText(layout, {std::round(tab.x + (tab.width - layout.size().x) / 2), origin.y},
                             current ? theme.text : theme.textMuted);
        }
        if (current && hasFocus()) {
            painter.strokeRect(tab.grownBy(-1, -1, -1, -1), Pen(theme.accent, 1.0f));
        }
        left += w[i];
    }
}

bool TabBar::onPointer(const PointerEvent &event) {
    const int tab = tabAt(event.position.x);
    if (event.type == PointerEvent::Type::Move || event.type == PointerEvent::Type::Leave) {
        const int hovered = event.type == PointerEvent::Type::Leave ? -1 : tab;
        if (hovered != m_hoveredTab) {
            m_hoveredTab = hovered;
            invalidatePaint();
        }
        return false;
    }
    if (event.type == PointerEvent::Type::Press && event.button == PointerButton::Left && tab >= 0) {
        choose(tab);
        return true;
    }
    return event.type == PointerEvent::Type::Release && isPressed();
}

bool TabBar::onKey(const KeyEvent &event) {
    if (event.type == KeyEvent::Type::Press && (event.key == Key::Left || event.key == Key::Right)) {
        choose(m_current + (event.key == Key::Right ? 1 : -1));
        return true;
    }
    return false;
}

// ---- TreeView ---------------------------------------------------------------------

TreeView::TreeView(TreeModel &model) : m_model(model) {
    setRole(Role::Tree);
    setFocusable(true);
    setClipsChildren(true);
    m_modelChanged = m_model.changed.connect([this] { rebuild(); });
    rebuild();
}

TreeView::~TreeView() = default;

float TreeView::rowHeight() const { return std::round(theme().controlHeight - 4.0f); }

void TreeView::addRows(TreeModel::Id parent, std::uint32_t depth) {
    const std::size_t count = m_model.childCount(parent);
    for (std::size_t i = 0; i < count; ++i) {
        const TreeModel::Id id = m_model.child(parent, i);
        const bool hasChildren = m_model.childCount(id) > 0;
        m_rows.push_back({id, parent, depth, hasChildren});
        if (hasChildren && m_expanded.contains(id)) {
            addRows(id, depth + 1);
        }
    }
}

void TreeView::rebuild() {
    m_rows.clear();
    addRows(TreeModel::kRoot, 0);
    if (rowOf(m_current) < 0) {
        m_current = TreeModel::kRoot;
    }
    m_scroll = std::clamp(m_scroll, 0.0f, maxScroll());
    invalidateLayout();
    invalidatePaint();
}

std::ptrdiff_t TreeView::rowOf(TreeModel::Id id) const {
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].id == id) {
            return std::ptrdiff_t(i);
        }
    }
    return -1;
}

void TreeView::setExpanded(TreeModel::Id node, bool expanded) {
    if (expanded == m_expanded.contains(node)) {
        return;
    }
    if (expanded) {
        m_expanded.insert(node);
    } else {
        m_expanded.erase(node);
    }
    rebuild();
}

void TreeView::setExpanded(const std::vector<TreeModel::Id> &nodes, bool expanded) {
    for (const TreeModel::Id node : nodes) {
        if (expanded) {
            m_expanded.insert(node);
        } else {
            m_expanded.erase(node);
        }
    }
    rebuild();
}

void TreeView::setSelection(std::vector<TreeModel::Id> selection) {
    m_selected = std::set<TreeModel::Id>(selection.begin(), selection.end());
    m_current = m_anchor = selection.empty() ? TreeModel::kRoot : selection.back();
    invalidatePaint();
}

std::vector<TreeModel::Id> TreeView::selection() const {
    std::vector<TreeModel::Id> out;
    for (const Row &row : m_rows) { // in tree order
        if (m_selected.contains(row.id)) {
            out.push_back(row.id);
        }
    }
    for (const TreeModel::Id id : m_selected) { // collapsed ones last
        if (rowOf(id) < 0) {
            out.push_back(id);
        }
    }
    return out;
}

void TreeView::reveal(const std::vector<TreeModel::Id> &path) {
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        m_expanded.insert(path[i]);
    }
    rebuild();
    if (!path.empty()) {
        const std::ptrdiff_t row = rowOf(path.back());
        if (row >= 0) {
            ensureRowVisible(std::size_t(row));
        }
    }
}

float TreeView::maxScroll() const { return std::max(0.0f, float(m_rows.size()) * rowHeight() - rect().height); }

void TreeView::scrollTo(float offset) {
    offset = std::clamp(offset, 0.0f, maxScroll());
    if (offset != m_scroll) {
        m_scroll = offset;
        invalidatePaint();
    }
}

void TreeView::ensureRowVisible(std::size_t row) {
    const float top = float(row) * rowHeight();
    if (top < m_scroll) {
        scrollTo(top);
    } else if (top + rowHeight() > m_scroll + rect().height) {
        scrollTo(top + rowHeight() - rect().height);
    }
}

Vec2 TreeView::measureContent(Vec2 available) {
    return {std::min(available.x, 200.0f), std::min(available.y, float(m_rows.size()) * rowHeight())};
}

void TreeView::arrangeContent(const RectF &) { m_scroll = std::clamp(m_scroll, 0.0f, maxScroll()); }

void TreeView::setCurrentRow(std::size_t row, bool extend, bool toggle) {
    if (m_rows.empty()) {
        return;
    }
    row = std::min(row, m_rows.size() - 1);
    const TreeModel::Id id = m_rows[row].id;
    if (toggle) {
        if (!m_selected.erase(id)) {
            m_selected.insert(id);
        }
        m_anchor = id;
    } else if (extend && rowOf(m_anchor) >= 0) {
        const std::size_t anchorRow = std::size_t(rowOf(m_anchor));
        const std::size_t from = std::min(anchorRow, row);
        const std::size_t to = std::max(anchorRow, row);
        m_selected.clear();
        for (std::size_t i = from; i <= to; ++i) {
            m_selected.insert(m_rows[i].id);
        }
    } else {
        m_selected = {id};
        m_anchor = id;
    }
    m_current = id;
    ensureRowVisible(row);
    invalidatePaint();
    selectionChanged.emit();
}

void TreeView::paint(Painter &painter, const Theme &theme) {
    const RectF r = rect();
    painter.fillRect(r, theme.panel);
    painter.save();
    painter.clipRect(r);
    const float h = rowHeight();
    const std::size_t first = std::size_t(std::max(0.0f, std::floor(m_scroll / h)));
    const std::size_t last = std::min(m_rows.size(), std::size_t(std::ceil((m_scroll + r.height) / h)));
    TextLayout layout;
    m_painted = 0;
    for (std::size_t i = first; i < last; ++i) {
        const Row &row = m_rows[i];
        const RectF line{r.x, std::round(r.y + float(i) * h - m_scroll), r.width, h};
        const bool selected = m_selected.contains(row.id);
        if (selected) {
            Color fill = theme.accent;
            fill.a = hasFocus() ? 0.55f : 0.3f;
            painter.fillRect(line, fill);
        }
        if (row.id == m_current && hasFocus()) {
            painter.strokeRect(line.grownBy(-0.5f, -0.5f, -0.5f, -0.5f), Pen(theme.accent, 1.0f));
        }
        const float x = line.x + 4.0f + float(row.depth) * kIndent;
        if (row.hasChildren) {
            triangle(painter, {x + 6.0f, line.y + h / 2.0f}, m_expanded.contains(row.id), theme.textMuted);
        }
        if (layoutIn(layout, m_model.text(row.id), theme, kInfinity, false, TextAlign::Start)) {
            const Vec2 origin = centredOrigin(layout, line);
            painter.drawText(layout, {x + 16.0f, origin.y}, theme.text);
        }
        ++m_painted;
    }
    painter.restore();
}

bool TreeView::onPointer(const PointerEvent &event) {
    const RectF r = rect();
    if (event.type == PointerEvent::Type::Wheel) {
        const float before = m_scroll;
        scrollTo(m_scroll - (event.wheelDelta.y != 0.0f ? event.wheelDelta.y : 0.0f));
        return m_scroll != before;
    }
    if (event.type != PointerEvent::Type::Press) {
        return event.type == PointerEvent::Type::Release && isPressed();
    }
    const float y = event.position.y - r.y + m_scroll;
    const std::size_t index = std::size_t(std::max(0.0f, y / rowHeight()));
    if (index >= m_rows.size()) {
        if (!m_selected.empty()) {
            m_selected.clear();
            invalidatePaint();
            selectionChanged.emit();
        }
        return true;
    }
    const Row row = m_rows[index];
    const float arrowX = r.x + 4.0f + float(row.depth) * kIndent;
    if (row.hasChildren && event.position.x >= arrowX && event.position.x < arrowX + 14.0f) {
        setExpanded(row.id, !isExpanded(row.id));
        return true;
    }
    if (event.button == PointerButton::Right && m_selected.contains(row.id)) {
        return false; // keep the selection for a context menu
    }
    setCurrentRow(index, hasModifier(event.modifiers, Modifier::Shift), ctrlHeld(event.modifiers));
    if (event.clickCount >= 2 && event.button == PointerButton::Left) {
        activated.emit(row.id);
    }
    return event.button == PointerButton::Left;
}

bool TreeView::onKey(const KeyEvent &event) {
    if (event.type != KeyEvent::Type::Press || m_rows.empty()) {
        return false;
    }
    const std::ptrdiff_t at = rowOf(m_current);
    const std::size_t row = at < 0 ? 0 : std::size_t(at);
    const bool shift = hasModifier(event.modifiers, Modifier::Shift);
    const std::size_t page = std::max<std::size_t>(1, std::size_t(rect().height / rowHeight()) - 1);
    switch (event.key) {
    case Key::Down: setCurrentRow(at < 0 ? 0 : row + 1, shift, false); return true;
    case Key::Up: setCurrentRow(row > 0 ? row - 1 : 0, shift, false); return true;
    case Key::Home: setCurrentRow(0, shift, false); return true;
    case Key::End: setCurrentRow(m_rows.size() - 1, shift, false); return true;
    case Key::PageDown: setCurrentRow(row + page, shift, false); return true;
    case Key::PageUp: setCurrentRow(row > page ? row - page : 0, shift, false); return true;
    case Key::Right:
        if (at >= 0 && m_rows[row].hasChildren) {
            if (!isExpanded(m_current)) {
                setExpanded(m_current, true);
            } else {
                setCurrentRow(row + 1, false, false);
            }
        }
        return true;
    case Key::Left:
        if (at >= 0) {
            if (isExpanded(m_current)) {
                setExpanded(m_current, false);
            } else if (m_rows[row].parent != TreeModel::kRoot) {
                setCurrentRow(std::size_t(rowOf(m_rows[row].parent)), false, false);
            }
        }
        return true;
    case Key::Enter:
        if (at >= 0) {
            activated.emit(m_current);
        }
        return true;
    default:
        return false;
    }
}

} // namespace cfw
