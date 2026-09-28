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

namespace {
constexpr float kIconSize = 16.0f;
constexpr float kDragThreshold = 4.0f;
} // namespace

TreeView::TreeView(TreeModel &model) : m_model(model) {
    setRole(Role::Tree);
    setFocusable(true);
    setClipsChildren(true);
    m_modelChanged = m_model.changed.connect([this] { rebuild(); });
    rebuild();
}

TreeView::~TreeView() = default;

float TreeView::rowHeight() const { return std::round(theme().controlHeight - 4.0f); }

bool TreeView::computeVisible(TreeModel::Id node) {
    bool visible = node != TreeModel::kRoot && m_filter(node);
    const std::size_t count = m_model.childCount(node);
    for (std::size_t i = 0; i < count; ++i) {
        // Every child is visited, so each node's answer is known.
        visible = computeVisible(m_model.child(node, i)) || visible;
    }
    m_visible[node] = visible;
    return visible;
}

void TreeView::addRows(TreeModel::Id parent, std::uint32_t depth) {
    const std::size_t count = m_model.childCount(parent);
    for (std::size_t i = 0; i < count; ++i) {
        const TreeModel::Id id = m_model.child(parent, i);
        if (m_filter && !m_visible[id]) {
            continue;
        }
        bool hasChildren = false;
        const std::size_t children = m_model.childCount(id);
        if (m_filter) {
            for (std::size_t c = 0; c < children && !hasChildren; ++c) {
                hasChildren = m_visible[m_model.child(id, c)];
            }
        } else {
            hasChildren = children > 0;
        }
        m_rowIndex[id] = m_rows.size();
        m_rows.push_back({id, parent, depth, hasChildren});
        // A filtered tree shows every match, so everything leading to one is open.
        if (hasChildren && (m_filter || m_expanded.contains(id))) {
            addRows(id, depth + 1);
        }
    }
}

void TreeView::rebuild() {
    m_rows.clear();
    m_rowIndex.clear();
    m_visible.clear();
    if (m_filter) {
        computeVisible(TreeModel::kRoot);
    }
    addRows(TreeModel::kRoot, 0);
    if (rowOf(m_current) < 0) {
        m_current = TreeModel::kRoot;
    }
    if (isRenaming() && rowOf(m_renaming) < 0) {
        cancelRename();
    }
    m_drop.reset();
    m_scroll = std::clamp(m_scroll, 0.0f, maxScroll());
    invalidateLayout();
    invalidatePaint();
}

std::ptrdiff_t TreeView::rowOf(TreeModel::Id id) const {
    const auto it = m_rowIndex.find(id);
    return it == m_rowIndex.end() ? -1 : std::ptrdiff_t(it->second);
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

void TreeView::setShowsExpanders(bool shows) {
    m_showsExpanders = shows;
    invalidatePaint();
}

void TreeView::setFilter(std::function<bool(TreeModel::Id)> matches) {
    m_filter = std::move(matches);
    m_scroll = 0.0f;
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

float TreeView::headerHeight() const {
    const bool titled = std::any_of(m_columns.begin(), m_columns.end(), [](const Column &c) { return !c.title.empty(); });
    return titled ? rowHeight() : 0.0f;
}

RectF TreeView::body() const {
    const float header = headerHeight();
    return {rect().x, rect().y + header, rect().width, std::max(0.0f, rect().height - header)};
}

void TreeView::setColumns(std::vector<Column> columns) {
    m_columns = std::move(columns);
    invalidateLayout();
    invalidatePaint();
}

float TreeView::columnWidth(std::size_t column) const {
    if (m_columns.size() <= 1) {
        return rect().width;
    }
    if (column + 1 < m_columns.size()) {
        return m_columns[column].width;
    }
    float used = 0.0f;
    for (std::size_t i = 0; i + 1 < m_columns.size(); ++i) {
        used += m_columns[i].width;
    }
    return std::max(40.0f, rect().width - used); // the last column takes the rest
}

bool TreeView::isAtBottom() const { return m_scroll >= maxScroll() - 0.5f; }

void TreeView::scrollToBottom() { scrollTo(maxScroll()); }

String TreeView::selectedText() const {
    String out;
    for (const TreeModel::Id id : selection()) {
        for (std::size_t c = 0; c < columnCount(); ++c) {
            if (c > 0) {
                out += '\t';
            }
            out += m_model.cellText(id, c);
        }
        out += '\n';
    }
    return out;
}

std::ptrdiff_t TreeView::columnBorderAt(Vec2 position) const {
    if (m_columns.size() <= 1 || position.y < rect().y || position.y >= rect().y + headerHeight()) {
        return -1;
    }
    float x = rect().x;
    for (std::size_t i = 0; i + 1 < m_columns.size(); ++i) {
        x += m_columns[i].width;
        if (std::abs(position.x - x) <= 3.0f) {
            return std::ptrdiff_t(i);
        }
    }
    return -1;
}

std::optional<Cursor> TreeView::cursorAt(Vec2 position) const {
    if (m_resizing >= 0 || columnBorderAt(position) >= 0) {
        return Cursor::SizeHorizontal;
    }
    return Element::cursorAt(position);
}

float TreeView::maxScroll() const { return std::max(0.0f, float(m_rows.size()) * rowHeight() - body().height); }

void TreeView::scrollTo(float offset) {
    offset = std::clamp(offset, 0.0f, maxScroll());
    if (offset != m_scroll) {
        m_scroll = offset;
        invalidateLayout(); // the rename field moves with its row
        invalidatePaint();
    }
}

void TreeView::ensureRowVisible(std::size_t row) {
    const float top = float(row) * rowHeight();
    if (top < m_scroll) {
        scrollTo(top);
    } else if (top + rowHeight() > m_scroll + body().height) {
        scrollTo(top + rowHeight() - body().height);
    }
}

RectF TreeView::rowRect(std::size_t row) const {
    const float h = rowHeight();
    return {rect().x, std::round(body().y + float(row) * h - m_scroll), rect().width, h};
}

std::ptrdiff_t TreeView::rowAt(float y) const {
    if (y < body().y) {
        return -1; // the header
    }
    const float offset = y - body().y + m_scroll;
    if (offset < 0.0f) {
        return -1;
    }
    const std::size_t index = std::size_t(offset / rowHeight());
    return index < m_rows.size() ? std::ptrdiff_t(index) : -1;
}

float TreeView::textX(const Row &row) const {
    float x = rect().x + 4.0f + float(row.depth) * kIndent + (m_showsExpanders ? 16.0f : 0.0f);
    if (!m_model.icon(row.id).isNull()) {
        x += kIconSize + 4.0f;
    }
    return x;
}

Vec2 TreeView::measureContent(Vec2 available) {
    return {std::min(available.x, 200.0f), std::min(available.y, float(m_rows.size()) * rowHeight() + headerHeight())};
}

void TreeView::arrangeContent(const RectF &) {
    m_scroll = std::clamp(m_scroll, 0.0f, maxScroll());
    if (m_editor && isRenaming()) {
        const std::ptrdiff_t row = rowOf(m_renaming);
        if (row >= 0) {
            const RectF line = rowRect(std::size_t(row));
            const float x = textX(m_rows[std::size_t(row)]) - 4.0f;
            const float right = rect().x + columnWidth(0);
            m_editor->arrange({x, line.y, std::max(40.0f, right - x - 2.0f), line.height});
        }
    }
}

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

// ---- Renaming ----

void TreeView::startRename(TreeModel::Id node) {
    if (!m_model.canRename(node)) {
        return;
    }
    const std::ptrdiff_t row = rowOf(node);
    if (row < 0) {
        return;
    }
    ensureRowVisible(std::size_t(row));
    if (!m_editor) {
        // One field, reused: committing happens inside its own signals.
        m_editor = &add<TextField>();
        m_editorConnections.push_back(m_editor->submitted.connect([this](const String &) { commitRename(); }));
        m_editorConnections.push_back(m_editor->focusLost.connect([this] { commitRename(); }));
    }
    m_renaming = node;
    m_editor->setText(m_model.text(node));
    m_editor->selectAll();
    m_editor->setVisible(true);
    invalidateLayout();
    if (Surface *s = surface()) {
        s->setFocus(m_editor);
    }
}

void TreeView::commitRename() {
    if (!isRenaming()) {
        return;
    }
    const TreeModel::Id node = m_renaming;
    const String text = m_editor->text();
    cancelRename();
    if (!text.empty() && text != m_model.text(node)) {
        renamed.emit(node, text);
    }
}

void TreeView::cancelRename() {
    if (!isRenaming()) {
        return;
    }
    m_renaming = TreeModel::kRoot; // first: hiding the field ends its editing
    const bool hadFocus = m_editor->hasFocus();
    m_editor->setVisible(false);
    if (hadFocus) {
        if (Surface *s = surface()) {
            s->setFocus(this);
        }
    }
    invalidatePaint();
}

// ---- Drag and drop ----

std::ptrdiff_t TreeView::indexInParent(TreeModel::Id parent, TreeModel::Id node) const {
    const std::size_t count = m_model.childCount(parent);
    for (std::size_t i = 0; i < count; ++i) {
        if (m_model.child(parent, i) == node) {
            return std::ptrdiff_t(i);
        }
    }
    return -1;
}

std::optional<TreeView::Drop> TreeView::dropAt(Vec2 position) const {
    const std::vector<TreeModel::Id> nodes = selection();
    if (nodes.empty()) {
        return std::nullopt;
    }
    const std::ptrdiff_t at = rowAt(position.y);
    if (at < 0) {
        // Below the last row: at the end of the top level.
        Drop drop{TreeModel::kRoot, std::ptrdiff_t(m_model.childCount(TreeModel::kRoot)), -1, Drop::Zone::After};
        if (m_model.canDrop(nodes, drop.parent, drop.index)) {
            return drop;
        }
        return std::nullopt;
    }
    const Row &row = m_rows[std::size_t(at)];
    const RectF line = rowRect(std::size_t(at));
    const float f = (position.y - line.y) / line.height;
    const std::ptrdiff_t index = indexInParent(row.parent, row.id);
    // An open parent's lower edge is the top of its first child: "into".
    const bool openParent = row.hasChildren && (m_filter || m_expanded.contains(row.id));
    std::vector<Drop> tries;
    if (f < 0.25f) {
        tries = {{row.parent, index, at, Drop::Zone::Before}, {row.id, -1, at, Drop::Zone::Onto}};
    } else if (f > 0.75f && !openParent) {
        tries = {{row.parent, index + 1, at, Drop::Zone::After}, {row.id, -1, at, Drop::Zone::Onto}};
    } else {
        tries = {{row.id, -1, at, Drop::Zone::Onto}};
    }
    for (const Drop &drop : tries) {
        // Never onto (or into) a dragged node itself.
        if (drop.zone == Drop::Zone::Onto && std::find(nodes.begin(), nodes.end(), row.id) != nodes.end()) {
            continue;
        }
        if (m_model.canDrop(nodes, drop.parent, drop.index)) {
            return drop;
        }
    }
    return std::nullopt;
}

// ---- Painting and input ----

void TreeView::paint(Painter &painter, const Theme &theme) {
    const RectF r = rect();
    painter.fillRect(r, theme.panel);
    painter.save();
    painter.clipRect(body());
    const float h = rowHeight();
    const std::size_t first = std::size_t(std::max(0.0f, std::floor(m_scroll / h)));
    const std::size_t last = std::min(m_rows.size(), std::size_t(std::ceil((m_scroll + body().height) / h)));
    TextLayout layout;
    m_painted = 0;
    const auto drawCell = [&](StringView text, const TreeModel::CellStyle &style, float x, float right,
                              const RectF &line) {
        const float width = right - x - 4.0f;
        if (width <= 0.0f || text.empty()) {
            return;
        }
        Theme cellTheme = theme;
        if (style.mono && theme.monoFont) {
            cellTheme.font = theme.monoFont;
        }
        if (!cellTheme.font) {
            return;
        }
        TextStyle textStyle;
        textStyle.font = cellTheme.font;
        textStyle.pixelSize = theme.fontSize;
        textStyle.fallback = theme.fonts.get();
        TextLayoutOptions options;
        options.maxWidth = width;
        options.wrap = false;
        options.elide = true;
        options.maxLines = 1;
        layout.setText(text);
        layout.layout(textStyle, options);
        const Vec2 origin = centredOrigin(layout, line);
        painter.drawText(layout, {x, origin.y},
                         !isEnabled() ? theme.textDisabled : style.color.value_or(theme.text));
    };
    for (std::size_t i = first; i < last; ++i) {
        const Row &row = m_rows[i];
        const RectF line = rowRect(i);
        const bool selected = m_selected.contains(row.id);
        if (selected) {
            Color fill = theme.accent;
            fill.a = hasFocus() ? 0.55f : 0.3f;
            painter.fillRect(line, fill);
        }
        if (m_drop && m_drop->row == std::ptrdiff_t(i) && m_drop->zone == Drop::Zone::Onto) {
            painter.strokeRect(line.grownBy(-1.0f, -1.0f, -1.0f, -1.0f), Pen(theme.accent, 2.0f));
        }
        if (row.id == m_current && hasFocus()) {
            painter.strokeRect(line.grownBy(-0.5f, -0.5f, -0.5f, -0.5f), Pen(theme.accent, 1.0f));
        }
        float x = line.x + 4.0f + float(row.depth) * kIndent;
        if (m_showsExpanders) {
            if (row.hasChildren) {
                triangle(painter, {x + 6.0f, line.y + h / 2.0f}, m_filter || m_expanded.contains(row.id),
                         theme.textMuted);
            }
            x += 16.0f;
        }
        const Icon icon = m_model.icon(row.id);
        if (!icon.isNull()) {
            icon.paint(painter, {x, std::round(line.y + (h - kIconSize) / 2.0f), kIconSize, kIconSize}, theme.text);
            x += kIconSize + 4.0f;
        }
        if (row.id != m_renaming) {
            drawCell(m_model.cellText(row.id, 0), m_model.cellStyle(row.id, 0), x, r.x + columnWidth(0), line);
        }
        float cellX = r.x + columnWidth(0);
        for (std::size_t c = 1; c < columnCount(); ++c) {
            const float right = cellX + columnWidth(c);
            drawCell(m_model.cellText(row.id, c), m_model.cellStyle(row.id, c), cellX + 6.0f, right, line);
            cellX = right;
        }
        ++m_painted;
    }
    if (m_drop && m_drop->zone != Drop::Zone::Onto) {
        // A line where the rows will go, indented to their depth.
        float y = 0.0f;
        std::uint32_t depth = 0;
        if (m_drop->row < 0) {
            y = rowRect(m_rows.size()).y;
        } else {
            const RectF line = rowRect(std::size_t(m_drop->row));
            y = m_drop->zone == Drop::Zone::Before ? line.y : line.bottom();
            depth = m_rows[std::size_t(m_drop->row)].depth;
        }
        const float x = r.x + 4.0f + float(depth) * kIndent + (m_showsExpanders ? 16.0f : 0.0f);
        painter.drawLine({x, std::round(y) + 0.5f}, {r.right() - 4.0f, std::round(y) + 0.5f}, Pen(theme.accent, 2.0f));
    }
    painter.restore();

    if (headerHeight() > 0.0f) {
        const RectF header{r.x, r.y, r.width, headerHeight()};
        painter.fillRect(header, theme.window);
        painter.drawLine({r.x, header.bottom() - 0.5f}, {r.right(), header.bottom() - 0.5f}, Pen(theme.border, 1.0f));
        float x = r.x;
        for (std::size_t c = 0; c < m_columns.size(); ++c) {
            const float w = columnWidth(c);
            drawCell(m_columns[c].title, {theme.textMuted, false}, x + 6.0f, x + w, header);
            if (c + 1 < m_columns.size()) {
                painter.drawLine({x + w - 0.5f, header.y + 4.0f}, {x + w - 0.5f, header.bottom() - 4.0f},
                                 Pen(theme.border, 1.0f));
            }
            x += w;
        }
    }
}

bool TreeView::onPointer(const PointerEvent &event) {
    const RectF r = rect();
    if (m_resizing >= 0) {
        if (event.type == PointerEvent::Type::Move) {
            float left = r.x;
            for (std::ptrdiff_t i = 0; i < m_resizing; ++i) {
                left += m_columns[std::size_t(i)].width;
            }
            m_columns[std::size_t(m_resizing)].width = std::max(30.0f, event.position.x - m_resizeFrom - left);
            invalidatePaint();
        } else if (event.type == PointerEvent::Type::Release) {
            m_resizing = -1;
        }
        return true;
    }
    if (event.type == PointerEvent::Type::Press && event.position.y < body().y) {
        // The header: its borders resize columns; the rest does nothing.
        const std::ptrdiff_t border = columnBorderAt(event.position);
        if (border >= 0 && event.button == PointerButton::Left) {
            float x = r.x;
            for (std::ptrdiff_t i = 0; i <= border; ++i) {
                x += m_columns[std::size_t(i)].width;
            }
            m_resizing = border;
            m_resizeFrom = event.position.x - x;
        }
        return event.button == PointerButton::Left;
    }
    switch (event.type) {
    case PointerEvent::Type::Wheel: {
        const float before = m_scroll;
        scrollTo(m_scroll - (event.wheelDelta.y != 0.0f ? event.wheelDelta.y : 0.0f));
        return m_scroll != before;
    }
    case PointerEvent::Type::Move: {
        if (!isPressed() || m_pressRow < 0) {
            return false;
        }
        if (!m_dragging) {
            const Vec2 d = event.position - m_pressAt;
            const std::vector<TreeModel::Id> nodes = selection();
            const bool draggable =
                !nodes.empty() && std::all_of(nodes.begin(), nodes.end(), [this](TreeModel::Id id) {
                    return m_model.canDrag(id);
                });
            if (!draggable || d.length() < kDragThreshold) {
                return true;
            }
            m_dragging = true;
            m_selectOnRelease = false;
        }
        // Near the edges the view scrolls towards the pointer.
        if (event.position.y < body().y + rowHeight() / 2.0f) {
            scrollTo(m_scroll - rowHeight() / 2.0f);
        } else if (event.position.y > r.bottom() - rowHeight() / 2.0f) {
            scrollTo(m_scroll + rowHeight() / 2.0f);
        }
        m_drop = dropAt(event.position);
        invalidatePaint();
        return true;
    }
    case PointerEvent::Type::Release: {
        if (!isPressed()) {
            return false;
        }
        if (m_dragging) {
            const std::optional<Drop> drop = m_drop;
            m_dragging = false;
            m_drop.reset();
            invalidatePaint();
            if (drop) {
                dropped.emit(selection(), drop->parent, drop->index);
            }
        } else if (m_selectOnRelease && m_pressRow >= 0 && std::size_t(m_pressRow) < m_rows.size()) {
            setCurrentRow(std::size_t(m_pressRow), false, false);
        }
        m_selectOnRelease = false;
        m_pressRow = -1;
        return true;
    }
    case PointerEvent::Type::Press:
        break;
    default:
        return false;
    }

    if (isRenaming()) {
        commitRename(); // a press elsewhere in the tree ends the rename
    }
    const std::ptrdiff_t at = rowAt(event.position.y);
    if (at < 0) {
        if (!m_selected.empty() && event.button == PointerButton::Left) {
            m_selected.clear();
            invalidatePaint();
            selectionChanged.emit();
        }
        if (event.button == PointerButton::Right) {
            contextMenuRequested.emit(TreeModel::kRoot, event.position);
            return true;
        }
        return event.button == PointerButton::Left;
    }
    const std::size_t index = std::size_t(at);
    const Row row = m_rows[index];
    const float arrowX = r.x + 4.0f + float(row.depth) * kIndent;
    if (m_showsExpanders && row.hasChildren && !m_filter && event.button == PointerButton::Left &&
        event.position.x >= arrowX && event.position.x < arrowX + 14.0f) {
        setExpanded(row.id, !isExpanded(row.id));
        return true;
    }
    if (event.button == PointerButton::Right) {
        // A context menu works on the selection: a row outside it becomes it.
        if (!m_selected.contains(row.id)) {
            setCurrentRow(index, false, false);
        }
        contextMenuRequested.emit(row.id, event.position);
        return true;
    }
    if (event.button != PointerButton::Left) {
        return false;
    }
    m_pressAt = event.position;
    m_pressRow = at;
    const bool shift = hasModifier(event.modifiers, Modifier::Shift);
    const bool ctrl = ctrlHeld(event.modifiers);
    if (!shift && !ctrl && m_selected.contains(row.id) && m_selected.size() > 1) {
        // Keep a multi-selection until release: it may be about to be dragged.
        m_selectOnRelease = true;
        m_current = row.id;
        invalidatePaint();
    } else {
        setCurrentRow(index, shift, ctrl);
    }
    if (event.clickCount >= 2) {
        activated.emit(row.id);
    }
    return true;
}

bool TreeView::onKey(const KeyEvent &event) {
    if (event.type != KeyEvent::Type::Press) {
        return false;
    }
    if (isRenaming()) {
        // Keys the field did not take: only Escape means something here.
        if (event.key == Key::Escape) {
            cancelRename();
            return true;
        }
        return false;
    }
    if (m_rows.empty()) {
        return false;
    }
    const std::ptrdiff_t at = rowOf(m_current);
    const std::size_t row = at < 0 ? 0 : std::size_t(at);
    const bool shift = hasModifier(event.modifiers, Modifier::Shift);
    const std::size_t page = std::max<std::size_t>(1, std::size_t(body().height / rowHeight()) - 1);
    if (event.key == Key::C && hasModifier(event.modifiers, Modifier::Control) && !m_selected.empty()) {
        if (Surface *s = surface()) {
            s->setClipboardText(selectedText());
        }
        return true;
    }
    switch (event.key) {
    case Key::Down: setCurrentRow(at < 0 ? 0 : row + 1, shift, false); return true;
    case Key::Up: setCurrentRow(row > 0 ? row - 1 : 0, shift, false); return true;
    case Key::Home: setCurrentRow(0, shift, false); return true;
    case Key::End: setCurrentRow(m_rows.size() - 1, shift, false); return true;
    case Key::PageDown: setCurrentRow(row + page, shift, false); return true;
    case Key::PageUp: setCurrentRow(row > page ? row - page : 0, shift, false); return true;
    case Key::Right:
        if (at >= 0 && m_rows[row].hasChildren && !m_filter) {
            if (!isExpanded(m_current)) {
                setExpanded(m_current, true);
            } else {
                setCurrentRow(row + 1, false, false);
            }
        }
        return true;
    case Key::Left:
        if (at >= 0) {
            if (isExpanded(m_current) && !m_filter) {
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
    case Key::F2:
        if (at >= 0 && m_model.canRename(m_current)) {
            startRename(m_current);
            return true;
        }
        return false;
    default:
        return false;
    }
}

} // namespace cfw
