#pragma once

// The first cfw-ui controls. Each measures itself from the theme (font,
// padding, control height) and paints with cfw::Painter.

#include <optional>
#include <string>
#include <vector>

#include <functional>

#include "cfw/core/Signal.h"
#include "cfw/text/TextLayout.h"
#include "cfw/ui/Element.h"
#include "cfw/ui/Icon.h"

namespace cfw {

// Static text, one or more lines, optionally wrapped to its width.
class Label : public Element {
public:
    explicit Label(String text = {});
    void setText(String text);
    [[nodiscard]] const String &text() const noexcept { return m_text; }
    void setWrap(bool wrap);
    void setMuted(bool muted); // secondary text colour

    void paint(Painter &painter, const Theme &theme) override;

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    void layoutText(float width);
    String m_text;
    TextLayout m_layout;
    float m_layoutWidth = -1.0f;
    bool m_wrap = false;
    bool m_muted = false;
};

// A push button: click with the pointer (press and release inside), or
// Space/Enter while focused. Hover, pressed, focus and disabled states come
// from the theme.
//
// With an icon and no text it is a square icon button; flat, it has no fill
// at rest (a toolbar button); checkable, each click toggles it (a tool
// choice, a panel's visibility).
class Button : public Element {
public:
    explicit Button(String text = {});
    explicit Button(Icon icon, String text = {});
    void setText(String text);
    [[nodiscard]] const String &text() const noexcept { return m_text; }
    void setIcon(Icon icon);
    [[nodiscard]] const Icon &icon() const noexcept { return m_icon; }
    void setIconSize(float size);
    // Filled with the accent colour (the default action of a dialog).
    void setPrimary(bool primary);
    void setFlat(bool flat);
    void setCheckable(bool checkable) noexcept { m_checkable = checkable; }
    [[nodiscard]] bool isCheckable() const noexcept { return m_checkable; }
    void setChecked(bool checked); // does not emit toggled
    [[nodiscard]] bool isChecked() const noexcept { return m_checked; }

    Signal<> clicked;
    Signal<bool> toggled; // checkable buttons, after a click changed the state

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;
    void onHoverChanged(bool hovered) override;
    void onPressedChanged(bool pressed) override;
    void onFocusChanged(bool focused) override;

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    void activate();
    String m_text;
    Icon m_icon;
    float m_iconSize = 16.0f;
    TextLayout m_layout;
    bool m_laidOut = false;
    bool m_primary = false;
    bool m_flat = false;
    bool m_checkable = false;
    bool m_checked = false;
};

} // namespace cfw

namespace cfw {

// A single-line text field: caret and selection by pointer (click, drag,
// double-click selects all) and keyboard (arrows, Home/End, Shift to
// select, Ctrl+A), Backspace/Delete, clipboard (Ctrl+C/X/V through the
// surface), undo/redo (Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z), a placeholder, and
// horizontal scrolling that keeps the caret in view. Positions are code
// points.
class TextField : public Element {
public:
    explicit TextField(String text = {});

    void setText(StringView text);
    [[nodiscard]] String text() const;
    void setPlaceholder(String placeholder);
    void setReadOnly(bool readOnly) noexcept { m_readOnly = readOnly; }
    // Marks the text as not accepted (the border turns the error colour)
    // until cleared; editing does not clear it.
    void setInvalid(bool invalid);
    // Shows each character as a dot and refuses to copy or cut (a password
    // or an access token).
    void setMasked(bool masked);
    [[nodiscard]] bool isMasked() const noexcept { return m_masked; }
    [[nodiscard]] bool isInvalid() const noexcept { return m_invalid; }
    // Longest text allowed, in code points (0: no limit).
    void setMaxLength(std::size_t maxLength) noexcept { m_maxLength = maxLength; }

    [[nodiscard]] std::size_t caret() const noexcept { return m_caret; }
    [[nodiscard]] std::size_t anchor() const noexcept { return m_anchor; }
    void setSelection(std::size_t anchor, std::size_t caret);
    void selectAll();
    [[nodiscard]] String selectedText() const;

    Signal<const String &> textChanged; // every edit
    Signal<const String &> submitted;   // Enter
    Signal<> editingFinished;           // Enter, or focus lost after an edit
    Signal<> focusLost;                 // every time, edited or not

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;
    bool onText(const TextEvent &event) override;
    bool onComposition(const CompositionEvent &event) override;
    [[nodiscard]] std::optional<RectF> textInputArea() const override;
    void onFocusChanged(bool focused) override;
    void onHoverChanged(bool hovered) override;
    // What an input method is composing (shown underlined at the caret).
    [[nodiscard]] String composition() const;

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    struct State {
        std::u32string text;
        std::size_t caret;
        std::size_t anchor;
    };
    void replaceSelection(std::u32string_view with);
    void changed();
    void moveCaret(std::size_t to, bool extend);
    [[nodiscard]] std::size_t indexAt(float x);
    void relayout();
    [[nodiscard]] float textLeft() const;
    // The caret in the laid-out text (inside the composition while composing).
    [[nodiscard]] std::size_t shownCaret() const noexcept;
    void clearComposition();

    std::u32string m_text;
    std::u32string m_composition;       // an input method's, not yet committed
    std::size_t m_compositionCursor = 0; // in code points
    String m_placeholder;
    std::size_t m_caret = 0;
    std::size_t m_anchor = 0;
    std::size_t m_maxLength = 0;
    float m_scroll = 0.0f; // horizontal, pixels
    bool m_readOnly = false;
    bool m_invalid = false;
    bool m_masked = false;
    bool m_dirty = false;  // edited since focus or last Enter
    bool m_layoutValid = false;
    TextLayout m_layout;
    TextLayout m_placeholderLayout;
    std::vector<State> m_undo;
    std::vector<State> m_redo;
};

// Scrolls one content element vertically (the content is laid out at the
// area's width, less the scroll bar when one is shown): wheel, dragging the scroll bar thumb, clicking the track, and
// keys (PageUp/PageDown, Home/End) when it has focus inside. The content is
// measured with unbounded height and clipped to the area.
class ScrollArea : public Element {
public:
    ScrollArea();
    // The scrolled element (replaces any previous one).
    Element &setContent(std::unique_ptr<Element> content);
    template <class T, class... Args> T &setContent(Args &&...args) {
        return static_cast<T &>(setContent(std::make_unique<T>(std::forward<Args>(args)...)));
    }
    [[nodiscard]] Element *content() const noexcept { return m_content; }

    [[nodiscard]] Vec2 offset() const noexcept { return m_offset; }
    void scrollTo(Vec2 offset);
    // Scrolls just enough to show `rect` (in surface coordinates).
    void ensureVisible(const RectF &rect);
    [[nodiscard]] Vec2 maxOffset() const noexcept;

    Signal<Vec2> scrolled;

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;

    static constexpr float kBarWidth = 10.0f;

protected:
    Vec2 measureContent(Vec2 available) override;
    void arrangeContent(const RectF &rect) override;

private:
    [[nodiscard]] RectF viewport() const;
    [[nodiscard]] RectF thumb() const;
    [[nodiscard]] bool showsBar() const;

    Element *m_content = nullptr;
    Vec2 m_contentSize;
    Vec2 m_offset;
    std::optional<float> m_dragFrom; // pointer y minus thumb top, while dragging
};

} // namespace cfw

namespace cfw {

// A check box with a label: click or Space toggles it.
class CheckBox : public Element {
public:
    explicit CheckBox(String text = {}, bool checked = false);
    void setChecked(bool checked); // does not emit toggled; clears partial
    [[nodiscard]] bool isChecked() const noexcept { return m_checked; }
    // Neither checked nor not (several objects that disagree): a dash. A
    // click makes it checked.
    void setPartial(bool partial);
    [[nodiscard]] bool isPartial() const noexcept { return m_partial; }
    Signal<bool> toggled;

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;
    void onHoverChanged(bool) override { invalidatePaint(); }
    void onFocusChanged(bool) override { invalidatePaint(); }

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    void toggle();
    String m_text;
    TextLayout m_layout;
    bool m_laidOut = false;
    bool m_checked = false;
    bool m_partial = false;
};

// A numeric field (QDoubleSpinBox): type a value, or step it with Up/Down
// (Shift: 10 steps) and the wheel while focused. Values are clamped to the
// range and rounded to `decimals`; text that does not parse restores the
// last value when editing finishes.
class NumberField : public TextField {
public:
    explicit NumberField(double value = 0.0, int decimals = 2);
    void setRange(double minimum, double maximum);
    void setStep(double step) noexcept { m_step = step; }
    void setDecimals(int decimals);
    void setValue(double value); // does not emit valueChanged
    [[nodiscard]] double value() const noexcept { return m_value; }
    Signal<double> valueChanged;

    bool onKey(const KeyEvent &event) override;
    bool onPointer(const PointerEvent &event) override;

private:
    void commit(double value);
    void showValue();
    double m_value = 0.0;
    double m_minimum = -1e300;
    double m_maximum = 1e300;
    double m_step = 1.0;
    int m_decimals = 2;
    ScopedConnection m_finished;
};

// A list of items shown in a popup (Qt's QComboBox): click or Space/Enter
// opens it; Up/Down change the selection directly.
class Dropdown : public Element {
public:
    explicit Dropdown(std::vector<String> items = {}, int current = 0);
    ~Dropdown() override;
    void setItems(std::vector<String> items);
    [[nodiscard]] const std::vector<String> &items() const noexcept { return m_items; }
    // -1 chooses nothing (the placeholder shows); does not emit currentChanged.
    void setCurrentIndex(int index);
    // Shown, muted, while nothing is chosen.
    void setPlaceholder(String placeholder);
    // A search field at the top of the list filters it as one types; Enter
    // chooses the first match.
    void setSearchable(bool searchable, String placeholder = "Search...");
    [[nodiscard]] int currentIndex() const noexcept { return m_current; }
    [[nodiscard]] String currentText() const;
    [[nodiscard]] bool isOpen() const noexcept { return m_popup != nullptr; }
    void open();
    Signal<int> currentChanged;

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;
    void onHoverChanged(bool) override { invalidatePaint(); }
    void onFocusChanged(bool) override { invalidatePaint(); }

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    void choose(int index);
    std::vector<String> m_items;
    int m_current = 0;
    String m_placeholder;
    bool m_searchable = false;
    String m_searchPlaceholder;
    Element *m_popup = nullptr;
    Surface *m_popupSurface = nullptr;
};

class Menu;

// One row of a popup menu or dropdown list: an optional icon, text, optional
// shortcut text, highlighted under the pointer, activates on release or
// Enter. An item with a submenu opens it instead (hover, Enter or Right).
class MenuItem : public Element {
public:
    explicit MenuItem(String text, String shortcut = {});
    [[nodiscard]] const String &text() const noexcept { return m_text; }
    void setText(String text);
    void setShortcutText(String shortcut);
    void setIcon(Icon icon);
    void setChecked(bool checked) { m_checked = checked; invalidatePaint(); }
    [[nodiscard]] bool isChecked() const noexcept { return m_checked; }
    // Fills the submenu each time it opens.
    void setSubmenu(std::function<void(Menu &)> fill);
    [[nodiscard]] bool hasSubmenu() const noexcept { return static_cast<bool>(m_submenu); }
    Signal<> activated;

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;
    void onHoverChanged(bool hovered) override;
    void onFocusChanged(bool) override { invalidatePaint(); }

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    friend class Menu;
    [[nodiscard]] Menu *menu() const;
    String m_text;
    String m_shortcut;
    Icon m_icon;
    TextLayout m_layout;
    TextLayout m_shortcutLayout;
    bool m_checked = false;
    std::function<void(Menu &)> m_submenu;
};

// A popup menu: a column of MenuItems on a panel. Up/Down move between
// items; activating one closes the menu and every menu it was opened from.
// Left closes a submenu; Left and Right in a menu that is not a submenu go
// to `sideways` (a menu bar moves to its neighbouring menu).
class Menu : public Stack {
public:
    Menu();
    MenuItem &addItem(String text, std::function<void()> action, String shortcut = {});
    MenuItem &addItem(Icon icon, String text, std::function<void()> action, String shortcut = {});
    MenuItem &addSubmenu(String text, std::function<void(Menu &)> fill, Icon icon = {});
    void addSeparator();
    // Opens this menu as a popup of `surface` at `position`.
    static Menu &popup(Surface &surface, std::unique_ptr<Menu> menu, Vec2 position);

    // Focuses the first enabled item (a menu opened from the keyboard).
    void focusFirstItem();
    // Closes this menu, its submenus and the menus it came from.
    void closeAll();
    [[nodiscard]] Menu *parentMenu() const noexcept { return m_parentMenu; }
    [[nodiscard]] Menu *openSubmenu() const noexcept { return m_openSubmenu; }
    std::function<void(int direction)> sideways; // -1 left, +1 right

    void paint(Painter &painter, const Theme &theme) override;
    bool onKey(const KeyEvent &event) override;

private:
    friend class MenuItem;
    void openSubmenuOf(MenuItem &item, bool focusFirst);
    void closeSubmenu();
    Menu *m_parentMenu = nullptr;
    Menu *m_openSubmenu = nullptr;
    MenuItem *m_submenuItem = nullptr;
};

} // namespace cfw
