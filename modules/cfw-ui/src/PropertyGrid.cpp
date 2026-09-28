#include "cfw/ui/PropertyGrid.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

#include "ControlText.h"
#include "cfw/gfx/Painter.h"
#include "cfw/ui/Surface.h"

namespace cfw {

using detail::centredOrigin;

namespace {

constexpr float kDividerGrab = 4.0f;

bool containsIgnoringCase(StringView haystack, StringView needle) {
    if (needle.empty()) {
        return true;
    }
    const auto lower = [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); };
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        std::size_t k = 0;
        while (k < needle.size() && lower(haystack[i + k]) == lower(needle[k])) {
            ++k;
        }
        if (k == needle.size()) {
            return true;
        }
    }
    return false;
}

void layoutElided(TextLayout &layout, StringView text, const Theme &theme, float width) {
    if (!theme.font) {
        return;
    }
    TextStyle style;
    style.font = theme.font;
    style.pixelSize = theme.fontSize;
    style.fallback = theme.fonts.get();
    TextLayoutOptions options;
    options.maxWidth = std::max(1.0f, width);
    options.wrap = false;
    options.elide = true;
    options.maxLines = 1;
    layout.setText(text);
    layout.layout(style, options);
}

} // namespace

// ---- PropertyRow -----------------------------------------------------------------------

PropertyRow::PropertyRow(String name, std::unique_ptr<Element> editor) : m_name(std::move(name)) {
    setRole(Role::Group);
    setAccessibleName(m_name);
    m_editor = &add(std::move(editor));
    m_editor->setAccessibleName(m_name);
}

void PropertyRow::setScrub(std::function<void(float)> step, std::function<void()> finished) {
    m_scrub = std::move(step);
    m_scrubFinished = std::move(finished);
}

float PropertyRow::labelWidth() const {
    for (const Element *e = parent(); e; e = e->parent()) {
        if (const auto *grid = dynamic_cast<const PropertyGrid *>(e)) {
            return grid->labelWidth();
        }
    }
    return 120.0f;
}

bool PropertyRow::onLabel(Vec2 p) const {
    const float right = rect().x + labelWidth();
    return p.x >= rect().x && p.x < right - kDividerGrab && p.y >= rect().y && p.y < rect().bottom();
}

Vec2 PropertyRow::measureContent(Vec2 available) {
    const Theme &t = theme();
    const float label = labelWidth();
    const Vec2 editor = m_editor->measure({std::max(0.0f, available.x - label - t.spacing), t.controlHeight});
    return {label + editor.x + t.spacing, std::max(t.controlHeight, editor.y) + 4.0f};
}

void PropertyRow::arrangeContent(const RectF &r) {
    const Theme &t = theme();
    const float x = r.x + labelWidth();
    const float h = std::min(t.controlHeight, r.height - 4.0f);
    m_editor->arrange({x, r.y + (r.height - h) / 2.0f, std::max(0.0f, r.right() - x - t.spacing), h});
}

void PropertyRow::paint(Painter &painter, const Theme &theme) {
    const float width = labelWidth() - theme.padding - kDividerGrab;
    layoutElided(m_layout, m_name, theme, width);
    if (theme.font) {
        const RectF label{rect().x + theme.padding, rect().y, width, rect().height};
        const bool active = m_scrubFrom.has_value() || (isHovered() && isScrubbable());
        painter.drawText(m_layout, centredOrigin(m_layout, label),
                         !isEnabled() ? theme.textDisabled : active ? theme.text : theme.textMuted);
    }
}

std::optional<Cursor> PropertyRow::cursorAt(Vec2 position) const {
    if (m_scrubFrom || (isScrubbable() && onLabel(position))) {
        return Cursor::SizeHorizontal;
    }
    return Element::cursorAt(position);
}

bool PropertyRow::onPointer(const PointerEvent &event) {
    switch (event.type) {
    case PointerEvent::Type::Press:
        if (event.button != PointerButton::Left || !onLabel(event.position)) {
            return false;
        }
        if (isScrubbable()) {
            m_scrubFrom = event.position.x;
            invalidatePaint();
        } else if (m_editor->isFocusable()) {
            if (Surface *s = surface()) {
                s->setFocus(m_editor); // a click on the name edits the value
            }
        }
        return true;
    case PointerEvent::Type::Move:
        if (m_scrubFrom && isPressed()) {
            float delta = event.position.x - *m_scrubFrom;
            if (hasModifier(event.modifiers, Modifier::Shift)) {
                delta *= 0.1f;
            }
            m_scrubFrom = event.position.x;
            if (delta != 0.0f && m_scrub) {
                m_scrub(delta);
            }
            return true;
        }
        return false;
    case PointerEvent::Type::Release:
        if (m_scrubFrom) {
            m_scrubFrom.reset();
            invalidatePaint();
            if (m_scrubFinished) {
                m_scrubFinished();
            }
            return true;
        }
        return isPressed();
    default:
        return false;
    }
}

// ---- PropertySection -----------------------------------------------------------------

PropertySection::PropertySection(String title) : m_title(std::move(title)) {
    setRole(Role::Group);
    setAccessibleName(m_title);
}

PropertyRow &PropertySection::addRow(String name, std::unique_ptr<Element> editor) {
    return static_cast<PropertyRow &>(add(std::make_unique<PropertyRow>(std::move(name), std::move(editor))));
}

std::vector<PropertyRow *> PropertySection::rows() const {
    std::vector<PropertyRow *> out;
    for (const auto &child : children()) {
        out.push_back(static_cast<PropertyRow *>(child.get()));
    }
    return out;
}

void PropertySection::setExpanded(bool expanded) {
    if (expanded == m_expanded) {
        return;
    }
    m_expanded = expanded;
    invalidateLayout();
    invalidatePaint();
    expandedChanged.emit(expanded);
}

float PropertySection::headerHeight() const { return std::round(theme().controlHeight); }

Vec2 PropertySection::measureContent(Vec2 available) {
    float height = headerHeight();
    float width = 0.0f;
    if (m_expanded) {
        for (const auto &child : children()) {
            if (child->isVisible()) {
                const Vec2 size = child->measure(available);
                height += size.y;
                width = std::max(width, size.x);
            }
        }
    }
    return {width, height};
}

void PropertySection::arrangeContent(const RectF &r) {
    float y = r.y + headerHeight();
    for (const auto &child : children()) {
        if (m_expanded && child->isVisible()) {
            const float h = child->measure({r.width, std::numeric_limits<float>::infinity()}).y;
            child->arrange({r.x, y, r.width, h});
            y += h;
        } else {
            child->arrange({r.x, y, r.width, 0.0f}); // collapsed: nothing to paint or hit
        }
    }
}

void PropertySection::paint(Painter &painter, const Theme &theme) {
    const RectF header{rect().x, rect().y, rect().width, headerHeight()};
    painter.fillRect(header, theme.window);
    painter.drawLine({header.x, header.bottom() - 0.5f}, {header.right(), header.bottom() - 0.5f},
                     Pen(theme.border, 1.0f));
    // The disclosure triangle.
    PainterPath triangle;
    const Vec2 c{header.x + 12.0f, header.y + header.height / 2.0f};
    if (m_expanded) {
        triangle.moveTo({c.x - 4, c.y - 2});
        triangle.lineTo({c.x + 4, c.y - 2});
        triangle.lineTo({c.x, c.y + 3});
    } else {
        triangle.moveTo({c.x - 2, c.y - 4});
        triangle.lineTo({c.x + 3, c.y});
        triangle.lineTo({c.x - 2, c.y + 4});
    }
    triangle.close();
    painter.fillPath(triangle, theme.textMuted);
    if (detail::layoutIn(m_layout, m_title, theme, std::numeric_limits<float>::infinity(), false, TextAlign::Start)) {
        painter.drawText(m_layout, {header.x + 24.0f, centredOrigin(m_layout, header).y}, theme.text);
    }
}

bool PropertySection::onPointer(const PointerEvent &event) {
    const bool onHeader = event.position.y >= rect().y && event.position.y < rect().y + headerHeight();
    if (event.type == PointerEvent::Type::Press && event.button == PointerButton::Left && onHeader) {
        return true;
    }
    if (event.type == PointerEvent::Type::Release && isPressed()) {
        if (onHeader) {
            setExpanded(!m_expanded);
        }
        return true;
    }
    return false;
}

// ---- PropertyGrid ------------------------------------------------------------------------

PropertyGrid::PropertyGrid() {
    setRole(Role::List);
    setAccessibleName("properties");
    setContent<Stack>(Stack::Direction::Column, 0.0f);
}

Stack &PropertyGrid::column() const { return static_cast<Stack &>(*content()); }

PropertySection &PropertyGrid::addSection(String title) {
    auto &section = static_cast<PropertySection &>(column().add(std::make_unique<PropertySection>(std::move(title))));
    applyFilter();
    return section;
}

std::vector<PropertySection *> PropertyGrid::sections() const {
    std::vector<PropertySection *> out;
    for (const auto &child : column().children()) {
        out.push_back(static_cast<PropertySection *>(child.get()));
    }
    return out;
}

PropertySection *PropertyGrid::section(StringView title) const {
    for (PropertySection *s : sections()) {
        if (s->title() == title) {
            return s;
        }
    }
    return nullptr;
}

void PropertyGrid::clear() {
    // Take the children out one by one (removal keeps the surface's
    // hover/focus pointers valid).
    Stack &c = column();
    while (!c.children().empty()) {
        static_cast<void>(c.remove(*c.children().back()));
    }
    invalidateLayout();
}

void PropertyGrid::setFilter(StringView text) {
    m_filter = String(text);
    applyFilter();
}

void PropertyGrid::applyFilter() {
    // Leading and trailing spaces are not part of a search.
    StringView needle = m_filter;
    while (!needle.empty() && needle.front() == ' ') {
        needle.remove_prefix(1);
    }
    while (!needle.empty() && needle.back() == ' ') {
        needle.remove_suffix(1);
    }
    for (PropertySection *s : sections()) {
        bool any = false;
        for (PropertyRow *row : s->rows()) {
            const bool match = containsIgnoringCase(row->name(), needle);
            row->setVisible(match);
            any = any || match;
        }
        s->setVisible(any || needle.empty());
        s->invalidateLayout();
    }
    invalidateLayout();
    invalidatePaint();
}

void PropertyGrid::setLabelWidth(float width) {
    const float limit = std::max(60.0f, rect().width - 60.0f);
    width = std::clamp(width, 60.0f, rect().width > 0.0f ? limit : 10000.0f);
    if (width == m_labelWidth) {
        return;
    }
    m_labelWidth = width;
    column().invalidateLayout();
    for (PropertySection *s : sections()) {
        for (PropertyRow *row : s->rows()) {
            row->invalidateLayout();
        }
    }
    invalidatePaint();
}

bool PropertyGrid::onDivider(Vec2 p) const {
    const float x = rect().x + m_labelWidth;
    return std::abs(p.x - x) <= kDividerGrab && p.y >= rect().y && p.y < rect().bottom();
}

std::optional<Cursor> PropertyGrid::cursorAt(Vec2 position) const {
    if (m_resizing || onDivider(position)) {
        return Cursor::SizeHorizontal;
    }
    return ScrollArea::cursorAt(position);
}

bool PropertyGrid::onPointer(const PointerEvent &event) {
    switch (event.type) {
    case PointerEvent::Type::Press:
        if (event.button == PointerButton::Left && onDivider(event.position)) {
            m_resizing = true;
            return true;
        }
        break;
    case PointerEvent::Type::Move:
        if (m_resizing) {
            setLabelWidth(event.position.x - rect().x);
            return true;
        }
        break;
    case PointerEvent::Type::Release:
        if (m_resizing) {
            m_resizing = false;
            return true;
        }
        break;
    default:
        break;
    }
    return ScrollArea::onPointer(event);
}

} // namespace cfw
