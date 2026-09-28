#pragma once

// The element tree every cfw-ui control is built from (spec §4.8).
//
// - Ownership: an element owns its children. Elements live in a Surface,
//   which routes input and schedules layout and painting.
// - Layout is two passes: measure(available) returns the size an element
//   would like; arrange(rect) places it and its children. Measurements are
//   cached until invalidateLayout(), which marks only the element and its
//   ancestors, so one changed label does not re-measure its siblings.
// - Painting: paint() draws the element itself; the surface paints children
//   on top, clipped to their parent when clipsChildren() is set.
//   invalidatePaint() adds the element's rectangle to the surface's damage.
// - Accessibility: every element carries a role and a name from day one.
//
// Rectangles are in the surface's logical pixels.
//
// Threads: the surface's thread only. Allocates: children, cached layout.

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "cfw/core/Rect.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"
#include "cfw/core/Vec2.h"
#include "cfw/ui/Event.h"

namespace cfw {

class Painter;
class Surface;
struct Theme;

enum class Role : std::uint8_t {
    None, Group, Label, Button, CheckBox, TextField, List, ListItem, Tree, TreeItem, Menu, MenuItem, Tab, Dialog,
};

class Element {
public:
    Element();
    virtual ~Element();
    Element(const Element &) = delete;
    Element &operator=(const Element &) = delete;

    // ---- Tree ----
    Element &add(std::unique_ptr<Element> child);
    template <class T, class... Args> T &add(Args &&...args) {
        return static_cast<T &>(add(std::make_unique<T>(std::forward<Args>(args)...)));
    }
    // Detaches `child` and hands it back (null if it is not a child).
    std::unique_ptr<Element> remove(Element &child);
    [[nodiscard]] Element *parent() const noexcept { return m_parent; }
    [[nodiscard]] Span<const std::unique_ptr<Element>> children() const noexcept { return m_children; }
    [[nodiscard]] Surface *surface() const noexcept;

    // ---- Geometry and layout ----
    [[nodiscard]] const RectF &rect() const noexcept { return m_rect; }
    // A fixed size overrides what measure() would say, per axis (0 = not fixed).
    void setFixedSize(Vec2 size);
    [[nodiscard]] Vec2 fixedSize() const noexcept { return m_fixed; }
    // Share of spare space in a Stack (0: keep the measured size).
    void setStretch(float stretch);
    [[nodiscard]] float stretch() const noexcept { return m_stretch; }

    // Cached measure; calls measureContent() when invalid.
    [[nodiscard]] Vec2 measure(Vec2 available);
    // Places this element and its children.
    void arrange(const RectF &rect);
    void invalidateLayout();
    void invalidatePaint();

    // ---- State ----
    void setVisible(bool visible);
    [[nodiscard]] bool isVisible() const noexcept { return m_visible; }
    void setEnabled(bool enabled);
    // Enabled and every ancestor enabled.
    [[nodiscard]] bool isEnabled() const noexcept;
    void setFocusable(bool focusable) noexcept { m_focusable = focusable; }
    [[nodiscard]] bool isFocusable() const noexcept { return m_focusable; }
    [[nodiscard]] bool isHovered() const noexcept;
    [[nodiscard]] bool isPressed() const noexcept;
    [[nodiscard]] bool hasFocus() const noexcept;
    void setClipsChildren(bool clips) noexcept { m_clips = clips; }
    [[nodiscard]] bool clipsChildren() const noexcept { return m_clips; }

    // ---- Pointer shape and tooltip ----
    // The pointer's shape over this element (unset: the parent's, and Arrow
    // at the root).
    void setCursor(std::optional<Cursor> cursor) noexcept { m_cursor = cursor; }
    // The shape over `position`; controls with parts (a splitter's handle)
    // override it.
    [[nodiscard]] virtual std::optional<Cursor> cursorAt(Vec2 position) const;
    // Shown after the pointer rests on the element (empty: none; a child
    // without one shows its parent's).
    void setToolTip(String text) { m_toolTip = std::move(text); }
    [[nodiscard]] const String &toolTip() const noexcept { return m_toolTip; }

    // ---- Accessibility ----
    void setRole(Role role) noexcept { m_role = role; }
    [[nodiscard]] Role role() const noexcept { return m_role; }
    void setAccessibleName(String name) { m_accessibleName = std::move(name); }
    [[nodiscard]] const String &accessibleName() const noexcept { return m_accessibleName; }

    // ---- For subclasses and the surface ----
    // Paints the element itself (children are painted after, by the surface).
    virtual void paint(Painter &painter, const Theme &theme);
    // Paints over the element's children (a drop highlight, a focus ring).
    virtual void paintOverlay(Painter &painter, const Theme &theme);
    // Input. Return true when handled; unhandled events go to the parent.
    virtual bool onPointer(const PointerEvent &event);
    virtual bool onKey(const KeyEvent &event);
    virtual bool onText(const TextEvent &event);
    // Files dragged over the element: return true on Enter and Move where it
    // would take them, and on Drop when it took them. Default: refuses (the
    // surface then asks the parent).
    virtual bool onDrop(const DropEvent &event);
    virtual void onHoverChanged(bool hovered);
    virtual void onPressedChanged(bool pressed);
    virtual void onFocusChanged(bool focused);

protected:
    // The size this element wants inside `available` (default: the largest
    // child's, as children are stacked on top of each other).
    virtual Vec2 measureContent(Vec2 available);
    // Places the children inside `rect` (default: each fills it).
    virtual void arrangeContent(const RectF &rect);
    // The theme of the surface, or a default one outside a surface.
    [[nodiscard]] const Theme &theme() const;

private:
    friend class Surface;
    void setSurface(Surface *surface) noexcept;

    Element *m_parent = nullptr;
    Surface *m_surface = nullptr; // set on the root; children ask their parent
    std::vector<std::unique_ptr<Element>> m_children;
    RectF m_rect;
    Vec2 m_fixed;
    float m_stretch = 0.0f;
    std::optional<Vec2> m_measuredFor; // cache key
    Vec2 m_measured;
    Role m_role = Role::None;
    String m_accessibleName;
    String m_toolTip;
    std::optional<Cursor> m_cursor;
    bool m_visible = true;
    bool m_enabled = true;
    bool m_focusable = false;
    bool m_clips = false;
};

// Lays children out in a row or a column, with spacing and padding. Spare
// space goes to children with a stretch; the cross axis is filled.
class Stack : public Element {
public:
    enum class Direction : std::uint8_t { Row, Column };
    explicit Stack(Direction direction = Direction::Column, float spacing = -1.0f, float padding = 0.0f);

    void setSpacing(float spacing); // < 0: the theme's spacing
    void setPadding(float padding);

protected:
    Vec2 measureContent(Vec2 available) override;
    void arrangeContent(const RectF &rect) override;

private:
    [[nodiscard]] float gap() const;
    Direction m_direction;
    float m_spacing;
    float m_padding;
};

} // namespace cfw
