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
class Button : public Element {
public:
    explicit Button(String text = {});
    void setText(String text);
    [[nodiscard]] const String &text() const noexcept { return m_text; }
    // Filled with the accent colour (the default action of a dialog).
    void setPrimary(bool primary);

    Signal<> clicked;

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;
    void onHoverChanged(bool hovered) override;
    void onPressedChanged(bool pressed) override;
    void onFocusChanged(bool focused) override;

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    String m_text;
    TextLayout m_layout;
    bool m_laidOut = false;
    bool m_primary = false;
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

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;
    bool onText(const TextEvent &event) override;
    void onFocusChanged(bool focused) override;
    void onHoverChanged(bool hovered) override;

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

    std::u32string m_text;
    String m_placeholder;
    std::size_t m_caret = 0;
    std::size_t m_anchor = 0;
    std::size_t m_maxLength = 0;
    float m_scroll = 0.0f; // horizontal, pixels
    bool m_readOnly = false;
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
    void setChecked(bool checked); // does not emit toggled
    [[nodiscard]] bool isChecked() const noexcept { return m_checked; }
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
    void setCurrentIndex(int index); // does not emit currentChanged
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
    Element *m_popup = nullptr;
    Surface *m_popupSurface = nullptr;
};

// One row of a popup menu or dropdown list: text, optional shortcut text,
// highlighted under the pointer, activates on release or Enter.
class MenuItem : public Element {
public:
    explicit MenuItem(String text, String shortcut = {});
    [[nodiscard]] const String &text() const noexcept { return m_text; }
    void setChecked(bool checked) { m_checked = checked; invalidatePaint(); }
    Signal<> activated;

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;
    void onHoverChanged(bool) override { invalidatePaint(); }
    void onFocusChanged(bool) override { invalidatePaint(); }

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    String m_text;
    String m_shortcut;
    TextLayout m_layout;
    TextLayout m_shortcutLayout;
    bool m_checked = false;
};

// A popup menu: a column of MenuItems on a panel. Up/Down move between
// items; activating one closes the menu.
class Menu : public Stack {
public:
    Menu();
    MenuItem &addItem(String text, std::function<void()> action, String shortcut = {});
    void addSeparator();
    // Opens this menu as a popup of `surface` at `position`.
    static Menu &popup(Surface &surface, std::unique_ptr<Menu> menu, Vec2 position);

    void paint(Painter &painter, const Theme &theme) override;
    bool onKey(const KeyEvent &event) override;
};

} // namespace cfw
