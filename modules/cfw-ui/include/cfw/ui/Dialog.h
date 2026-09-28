#pragma once

// Modal dialogs inside the window (spec: popups are layers, not OS windows):
// a titled panel with content and a row of buttons, centred over a dimmed
// window; message boxes; and a colour picker (QColorDialog's job).

#include <functional>
#include <memory>
#include <vector>

#include "cfw/core/Color.h"
#include "cfw/core/Signal.h"
#include "cfw/text/TextLayout.h"
#include "cfw/ui/Controls.h"

namespace cfw {

class Surface;

// A dialog: title, body (a column to fill) and buttons at the bottom right.
// Enter presses the default button; Escape and the window's close cancel it
// (result -1). finished runs exactly once, after the dialog has closed.
class Dialog : public Element {
public:
    static constexpr int kCancelled = -1;

    explicit Dialog(String title);
    [[nodiscard]] const String &title() const noexcept { return m_title; }
    [[nodiscard]] Stack &body() const noexcept { return *m_body; }
    // A button that finishes the dialog with `result`. The default one is
    // drawn as primary and is what Enter presses.
    Button &addButton(String text, int result, bool isDefault = false);
    // Closes the dialog; finished gets `result`.
    void finish(int result);
    Signal<int> finished;
    void describeAccessible(AccessibleNode &node) const override;

    // Opens `dialog` modally over `surface`, centred.
    static Dialog &open(Surface &surface, std::unique_ptr<Dialog> dialog);

    void paint(Painter &painter, const Theme &theme) override;
    bool onKey(const KeyEvent &event) override;

protected:
    Vec2 measureContent(Vec2 available) override;
    void arrangeContent(const RectF &rect) override;

private:
    [[nodiscard]] float titleHeight() const;
    String m_title;
    TextLayout m_titleLayout;
    Stack *m_body = nullptr;
    Stack *m_buttons = nullptr;
    Button *m_default = nullptr;
    int m_result = kCancelled;
    bool m_finished = false;
};

// A message with buttons ("Save", "Don't Save", "Cancel"); `done` gets the
// index of the button pressed, or -1 when cancelled.
class MessageBox {
public:
    static Dialog &show(Surface &surface, String title, String text, std::vector<String> buttons,
                        std::function<void(int)> done = {}, int defaultButton = 0);
};

// Picks a colour: a saturation/value square, a hue strip, the old and new
// colours side by side, and a hex field.
class ColorPicker : public Element {
public:
    explicit ColorPicker(Color color = {1, 1, 1, 1});
    void setColor(Color color); // does not emit
    [[nodiscard]] Color color() const noexcept { return m_color; }
    [[nodiscard]] Hsv hsv() const noexcept { return m_hsv; }
    Signal<Color> colorChanged;

    // A modal dialog around a picker; `accepted` gets the colour on OK.
    static Dialog &open(Surface &surface, Color initial, String title, std::function<void(Color)> accepted);

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;

    static constexpr float kSquare = 200.0f;
    static constexpr float kStrip = 18.0f;

protected:
    Vec2 measureContent(Vec2 available) override;
    void arrangeContent(const RectF &rect) override;

private:
    enum class Drag : std::uint8_t { None, Square, Hue };
    [[nodiscard]] RectF square() const;
    [[nodiscard]] RectF strip() const;
    [[nodiscard]] RectF preview() const;
    void change(const Hsv &hsv);
    void pick(Vec2 position);
    Hsv m_hsv;
    Color m_color;
    Color m_original;
    Drag m_drag = Drag::None;
    TextField *m_hex = nullptr;
    std::vector<ScopedConnection> m_connections;
};

// A colour well: a swatch that opens a ColorPicker; colorChanged on OK.
class ColorSwatch : public Element {
public:
    explicit ColorSwatch(Color color = {1, 1, 1, 1});
    void setColor(Color color); // does not emit
    [[nodiscard]] Color color() const noexcept { return m_color; }
    void setDialogTitle(String title) { m_title = std::move(title); }
    Signal<Color> colorChanged;
    void describeAccessible(AccessibleNode &node) const override;
    bool accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) override;

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;
    void onHoverChanged(bool) override { invalidatePaint(); }
    void onFocusChanged(bool) override { invalidatePaint(); }

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    void openPicker();
    Color m_color;
    String m_title = "Select Colour";
    TextLayout m_layout;
};

} // namespace cfw
