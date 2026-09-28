#include "cfw/ui/Surface.h"

#include <algorithm>
#include <vector>

#include "cfw/gfx/Painter.h"

namespace cfw {

namespace {

bool contains(const Element &ancestor, const Element *element) {
    for (const Element *e = element; e; e = e->parent()) {
        if (e == &ancestor) {
            return true;
        }
    }
    return false;
}

bool inside(const RectF &r, Vec2 p) { return p.x >= r.x && p.y >= r.y && p.x < r.right() && p.y < r.bottom(); }

void collectFocusable(Element &element, std::vector<Element *> &out) {
    if (!element.isVisible() || !element.isEnabled()) {
        return;
    }
    if (element.isFocusable()) {
        out.push_back(&element);
    }
    for (const auto &child : element.children()) {
        collectFocusable(*child, out);
    }
}

Element *hitTestIn(Element &element, Vec2 point) {
    if (!element.isVisible() || !inside(element.rect(), point)) {
        return nullptr;
    }
    const auto children = element.children();
    for (std::size_t i = children.size(); i-- > 0;) { // topmost (last painted) first
        if (Element *hit = hitTestIn(*children[i], point)) {
            return hit;
        }
    }
    return &element;
}

} // namespace

Surface::Surface(Theme theme) : m_theme(std::move(theme)) { setRoot(std::make_unique<Element>()); }

Surface::~Surface() = default;

void Surface::setRoot(std::unique_ptr<Element> root) {
    if (m_root) {
        elementRemoved(*m_root);
        m_root->setSurface(nullptr);
    }
    m_root = root ? std::move(root) : std::make_unique<Element>();
    m_root->setSurface(this);
    m_root->invalidateLayout();
    addDamage({0, 0, m_size.x, m_size.y});
}

void Surface::setSize(Vec2 size) {
    if (size == m_size) {
        return;
    }
    m_size = size;
    m_root->invalidateLayout();
    addDamage({0, 0, size.x, size.y});
}

void Surface::setTheme(Theme theme) {
    m_theme = std::move(theme);
    // Everything measures from the theme, so everything is invalid.
    std::vector<Element *> stack{m_root.get()};
    while (!stack.empty()) {
        Element *e = stack.back();
        stack.pop_back();
        e->m_measuredFor.reset();
        for (const auto &child : e->children()) {
            stack.push_back(child.get());
        }
    }
    m_layoutDirty = true;
    addDamage({0, 0, m_size.x, m_size.y});
}

void Surface::layout() {
    if (!m_layoutDirty) {
        return;
    }
    m_layoutDirty = false;
    static_cast<void>(m_root->measure(m_size));
    m_root->arrange({0, 0, m_size.x, m_size.y});
}

void Surface::paint(Painter &painter) {
    layout();
    painter.fillRect({0, 0, m_size.x, m_size.y}, m_theme.window);
    paintTree(painter, *m_root);
}

void Surface::paintTree(Painter &painter, Element &element) {
    if (!element.isVisible() || element.rect().isEmpty()) {
        return;
    }
    element.paint(painter, m_theme);
    if (element.children().empty()) {
        return;
    }
    painter.save();
    if (element.clipsChildren()) {
        painter.clipRect(element.rect());
    }
    for (const auto &child : element.children()) {
        paintTree(painter, *child);
    }
    painter.restore();
}

RectF Surface::takeDamage() {
    const RectF damage = m_damage.intersected({0, 0, m_size.x, m_size.y});
    m_damage = {};
    return damage;
}

void Surface::addDamage(const RectF &rect) { m_damage = m_damage.united(rect); }

void Surface::elementRemoved(Element &element) {
    if (contains(element, m_hovered)) {
        m_hovered = nullptr;
    }
    if (contains(element, m_pressed)) {
        m_pressed = nullptr;
    }
    if (contains(element, m_focus)) {
        m_focus = nullptr;
    }
}

Element *Surface::hitTest(Vec2 point) {
    layout();
    return hitTestIn(*m_root, point);
}

void Surface::setHovered(Element *element) {
    if (element == m_hovered) {
        return;
    }
    Element *old = m_hovered;
    m_hovered = element;
    if (old) {
        old->onHoverChanged(false);
    }
    if (element) {
        element->onHoverChanged(true);
    }
}

void Surface::setFocus(Element *element) {
    if (element == m_focus) {
        return;
    }
    Element *old = m_focus;
    m_focus = element;
    if (old) {
        old->onFocusChanged(false);
    }
    if (element) {
        element->onFocusChanged(true);
    }
}

void Surface::focusNext(bool backwards) {
    std::vector<Element *> order;
    collectFocusable(*m_root, order);
    if (order.empty()) {
        return;
    }
    std::size_t index = 0;
    const auto current = std::find(order.begin(), order.end(), m_focus);
    if (current == order.end()) {
        index = backwards ? order.size() - 1 : 0;
    } else {
        const std::size_t at = std::size_t(current - order.begin());
        index = backwards ? (at + order.size() - 1) % order.size() : (at + 1) % order.size();
    }
    setFocus(order[index]);
}

bool Surface::dispatch(const PointerEvent &event) {
    layout();
    Element *hit = event.type == PointerEvent::Type::Leave ? nullptr : hitTestIn(*m_root, event.position);
    // Disabled elements are hover- and click-transparent to their enabled parent.
    while (hit && !hit->isEnabled()) {
        hit = hit->parent();
    }
    if (!m_pressed) {
        setHovered(hit);
    } else {
        // While a button is held only the pressed element can be hovered, so
        // dragging off a button and releasing does not click it.
        setHovered(contains(*m_pressed, hit) ? m_pressed : nullptr);
    }

    // While a button is held, the pressed element gets everything.
    Element *target = m_pressed ? m_pressed : hit;
    if (event.type == PointerEvent::Type::Press && !m_pressed) {
        // Focus goes to the nearest focusable element; pressing elsewhere clears it.
        Element *focusable = hit;
        while (focusable && !focusable->isFocusable()) {
            focusable = focusable->parent();
        }
        setFocus(focusable);
    }

    bool handled = false;
    Element *handler = nullptr;
    for (Element *e = target; e && !handled; e = e->parent()) {
        handled = e->onPointer(event);
        handler = handled ? e : nullptr;
    }

    if (event.type == PointerEvent::Type::Press && handler && !m_pressed) {
        m_pressed = handler;
        handler->onPressedChanged(true);
    } else if (event.type == PointerEvent::Type::Release && m_pressed) {
        Element *released = m_pressed;
        m_pressed = nullptr;
        released->onPressedChanged(false);
        setHovered(hit);
    }
    return handled;
}

bool Surface::dispatch(const KeyEvent &event) {
    for (Element *e = m_focus; e; e = e->parent()) {
        if (e->onKey(event)) {
            return true;
        }
    }
    if (event.type == KeyEvent::Type::Press && event.key == Key::Tab &&
        !hasModifier(event.modifiers, Modifier::Control)) {
        focusNext(hasModifier(event.modifiers, Modifier::Shift));
        return true;
    }
    return false;
}

bool Surface::dispatch(const TextEvent &event) {
    for (Element *e = m_focus; e; e = e->parent()) {
        if (e->onText(event)) {
            return true;
        }
    }
    return false;
}

} // namespace cfw
