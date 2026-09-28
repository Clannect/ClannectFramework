#include "cfw/ui/Element.h"

#include <algorithm>
#include <cmath>

#include "cfw/core/Contract.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Theme.h"

namespace cfw {

Element::Element() = default;
Element::~Element() = default;

Element &Element::add(std::unique_ptr<Element> child) {
    require(child != nullptr && child->m_parent == nullptr, "Element::add: a new, unparented child");
    child->m_parent = this;
    m_children.push_back(std::move(child));
    invalidateLayout();
    return *m_children.back();
}

std::unique_ptr<Element> Element::remove(Element &child) {
    const auto it = std::find_if(m_children.begin(), m_children.end(),
                                 [&child](const std::unique_ptr<Element> &c) { return c.get() == &child; });
    if (it == m_children.end()) {
        return nullptr;
    }
    if (Surface *s = surface()) {
        s->elementRemoved(child);
    }
    invalidatePaint();
    std::unique_ptr<Element> detached = std::move(*it);
    m_children.erase(it);
    detached->m_parent = nullptr;
    invalidateLayout();
    return detached;
}

Surface *Element::surface() const noexcept {
    const Element *e = this;
    while (e->m_parent) {
        e = e->m_parent;
    }
    return e->m_surface;
}

void Element::setSurface(Surface *surface) noexcept { m_surface = surface; }

void Element::setFixedSize(Vec2 size) {
    m_fixed = size;
    invalidateLayout();
}

void Element::setStretch(float stretch) {
    m_stretch = std::max(0.0f, stretch);
    invalidateLayout();
}

Vec2 Element::measure(Vec2 available) {
    if (m_measuredFor && *m_measuredFor == available) {
        return m_measured;
    }
    Vec2 size = m_visible ? measureContent(available) : Vec2{};
    if (m_fixed.x > 0.0f) {
        size.x = m_fixed.x;
    }
    if (m_fixed.y > 0.0f) {
        size.y = m_fixed.y;
    }
    m_measuredFor = available;
    m_measured = size;
    return size;
}

void Element::arrange(const RectF &rect) {
    if (!(rect == m_rect)) {
        invalidatePaint(); // the old place
        m_rect = rect;
        invalidatePaint(); // and the new one
    }
    arrangeContent(rect);
}

void Element::invalidateLayout() {
    for (Element *e = this; e; e = e->m_parent) {
        e->m_measuredFor.reset();
    }
    if (Surface *s = surface()) {
        s->layoutInvalidated();
    }
}

void Element::invalidatePaint() {
    if (Surface *s = surface()) {
        s->addDamage(m_rect);
    }
}

void Element::setVisible(bool visible) {
    if (m_visible == visible) {
        return;
    }
    if (!visible) {
        invalidatePaint();
        if (Surface *s = surface()) {
            s->elementRemoved(*this); // drops hover, press and focus inside it
        }
    }
    m_visible = visible;
    invalidateLayout();
    invalidatePaint();
}

void Element::setEnabled(bool enabled) {
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    if (!enabled) {
        if (Surface *s = surface()) {
            s->elementRemoved(*this);
        }
    }
    invalidatePaint();
}

bool Element::isEnabled() const noexcept {
    for (const Element *e = this; e; e = e->m_parent) {
        if (!e->m_enabled) {
            return false;
        }
    }
    return true;
}

bool Element::isHovered() const noexcept {
    const Surface *s = surface();
    return s && s->hovered() == this;
}

bool Element::isPressed() const noexcept {
    const Surface *s = surface();
    return s && s->pressed() == this;
}

bool Element::hasFocus() const noexcept {
    const Surface *s = surface();
    return s && s->focus() == this;
}

const Theme &Element::theme() const {
    if (const Surface *s = surface()) {
        return s->theme();
    }
    static const Theme fallback = Theme::dark();
    return fallback;
}

void Element::paint(Painter &, const Theme &) {}
bool Element::onPointer(const PointerEvent &) { return false; }
bool Element::onKey(const KeyEvent &) { return false; }
bool Element::onText(const TextEvent &) { return false; }
void Element::onHoverChanged(bool) {}
void Element::onPressedChanged(bool) {}
void Element::onFocusChanged(bool) {}

Vec2 Element::measureContent(Vec2 available) {
    Vec2 size;
    for (const auto &child : m_children) {
        const Vec2 c = child->measure(available);
        size = {std::max(size.x, c.x), std::max(size.y, c.y)};
    }
    return size;
}

void Element::arrangeContent(const RectF &rect) {
    for (const auto &child : m_children) {
        if (child->isVisible()) {
            child->arrange(rect);
        }
    }
}

// ---- Stack ------------------------------------------------------------------------

Stack::Stack(Direction direction, float spacing, float padding)
    : m_direction(direction), m_spacing(spacing), m_padding(padding) {
    setRole(Role::Group);
}

void Stack::setSpacing(float spacing) {
    m_spacing = spacing;
    invalidateLayout();
}

void Stack::setPadding(float padding) {
    m_padding = padding;
    invalidateLayout();
}

float Stack::gap() const { return m_spacing >= 0.0f ? m_spacing : theme().spacing; }

Vec2 Stack::measureContent(Vec2 available) {
    const bool row = m_direction == Direction::Row;
    const Vec2 inner{std::max(0.0f, available.x - 2 * m_padding), std::max(0.0f, available.y - 2 * m_padding)};
    float main = 0.0f;
    float cross = 0.0f;
    int count = 0;
    for (const auto &child : children()) {
        if (!child->isVisible()) {
            continue;
        }
        const Vec2 c = child->measure(inner);
        main += row ? c.x : c.y;
        cross = std::max(cross, row ? c.y : c.x);
        ++count;
    }
    main += gap() * float(std::max(0, count - 1));
    const Vec2 size = row ? Vec2{main, cross} : Vec2{cross, main};
    return {size.x + 2 * m_padding, size.y + 2 * m_padding};
}

void Stack::arrangeContent(const RectF &rect) {
    const bool row = m_direction == Direction::Row;
    const RectF inner = rect.grownBy(-m_padding, -m_padding, -m_padding, -m_padding);
    const Vec2 available{std::max(0.0f, inner.width), std::max(0.0f, inner.height)};

    std::vector<Element *> visible;
    std::vector<float> sizes;
    float used = 0.0f;
    float totalStretch = 0.0f;
    for (const auto &child : children()) {
        if (!child->isVisible()) {
            continue;
        }
        const Vec2 c = child->measure(available);
        visible.push_back(child.get());
        sizes.push_back(row ? c.x : c.y);
        used += sizes.back();
        totalStretch += child->stretch();
    }
    const float length = row ? available.x : available.y;
    used += gap() * float(visible.empty() ? 0 : visible.size() - 1);
    const float spare = std::max(0.0f, length - used);

    float cursor = row ? inner.x : inner.y;
    for (std::size_t i = 0; i < visible.size(); ++i) {
        float extent = sizes[i];
        if (totalStretch > 0.0f) {
            extent += spare * visible[i]->stretch() / totalStretch;
        }
        extent = std::round(extent);
        const RectF placed = row ? RectF{cursor, inner.y, extent, available.y}
                                 : RectF{inner.x, cursor, available.x, extent};
        visible[i]->arrange(placed);
        cursor += extent + gap();
    }
}

} // namespace cfw
