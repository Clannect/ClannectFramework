#pragma once

// The first cfw-ui controls. Each measures itself from the theme (font,
// padding, control height) and paints with cfw::Painter.

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
