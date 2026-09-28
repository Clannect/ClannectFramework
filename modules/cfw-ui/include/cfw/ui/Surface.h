#pragma once

// A surface hosts one element tree: a window's contents, a popup, or an
// offscreen canvas in tests. It owns the root, the theme and the size, and it
//
// - routes pointer input: hit-testing, hover, press capture (the pressed
//   element gets every pointer event until release), bubbling of unhandled
//   events to parents;
// - routes keys and text to the focused element (bubbling), and moves focus
//   with Tab / Shift+Tab in tree order;
// - lays the tree out when something invalidated it, and collects damage.
//
// Platform windows (cfw-platform) feed it events and paint it each frame
// that has damage; nothing here touches the OS.
//
// Threads: one thread.

#include <memory>

#include "cfw/core/Rect.h"
#include "cfw/ui/Element.h"
#include "cfw/ui/Theme.h"

namespace cfw {

class Surface {
public:
    explicit Surface(Theme theme = Theme::dark());
    ~Surface();
    Surface(const Surface &) = delete;
    Surface &operator=(const Surface &) = delete;

    // The root element; replace it to show different content.
    [[nodiscard]] Element &root() noexcept { return *m_root; }
    void setRoot(std::unique_ptr<Element> root);

    void setSize(Vec2 size);
    [[nodiscard]] Vec2 size() const noexcept { return m_size; }
    void setTheme(Theme theme);
    [[nodiscard]] const Theme &theme() const noexcept { return m_theme; }

    // Lays out if anything asked for it. Called by paint() and by input.
    void layout();
    [[nodiscard]] bool needsLayout() const noexcept { return m_layoutDirty; }
    // Paints the whole tree (after layout()).
    void paint(Painter &painter);
    // What changed since the last call (empty: nothing to repaint).
    [[nodiscard]] RectF takeDamage();

    // Input. Returns whether an element handled it.
    bool dispatch(const PointerEvent &event);
    bool dispatch(const KeyEvent &event);
    bool dispatch(const TextEvent &event);

    // The deepest visible element under `point`.
    [[nodiscard]] Element *hitTest(Vec2 point);
    [[nodiscard]] Element *hovered() const noexcept { return m_hovered; }
    [[nodiscard]] Element *pressed() const noexcept { return m_pressed; }
    [[nodiscard]] Element *focus() const noexcept { return m_focus; }
    void setFocus(Element *element);
    // Tab order: focusable, enabled, visible elements in tree order.
    void focusNext(bool backwards = false);

private:
    friend class Element;
    void layoutInvalidated() noexcept { m_layoutDirty = true; }
    void addDamage(const RectF &rect);
    void elementRemoved(Element &element);
    void setHovered(Element *element);
    void paintTree(Painter &painter, Element &element);

    Theme m_theme;
    std::unique_ptr<Element> m_root;
    Vec2 m_size;
    bool m_layoutDirty = true;
    RectF m_damage;
    Element *m_hovered = nullptr;
    Element *m_pressed = nullptr;
    Element *m_focus = nullptr;
};

} // namespace cfw
