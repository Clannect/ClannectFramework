#include "cfw/ui/Controls.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "cfw/gfx/Painter.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Theme.h"

namespace cfw {

namespace {

// Lays `text` out in the theme's font; false when the theme has no font.
bool layoutIn(TextLayout &layout, StringView text, const Theme &theme, float maxWidth, bool wrap, TextAlign align) {
    if (!theme.font) {
        return false;
    }
    TextStyle style;
    style.font = theme.font;
    style.pixelSize = theme.fontSize;
    style.fallback = theme.fonts.get();
    TextLayoutOptions options;
    options.maxWidth = maxWidth;
    options.wrap = wrap;
    options.align = align;
    layout.setText(text);
    layout.layout(style, options);
    return true;
}

// The first baseline on a whole pixel, the text block centred vertically.
Vec2 centredOrigin(const TextLayout &layout, const RectF &rect) {
    const auto lines = layout.lines();
    if (lines.empty()) {
        return rect.origin();
    }
    const auto &last = lines[lines.size() - 1];
    const float height = last.top + last.ascent + last.descent;
    const float top = rect.y + (rect.height - height) / 2.0f;
    const float baseline = lines[0].baseline;
    return {rect.x, std::round(top + baseline) - baseline};
}

} // namespace

// ---- Label ------------------------------------------------------------------------

Label::Label(String text) : m_text(std::move(text)) {
    setRole(Role::Label);
    setAccessibleName(m_text);
}

void Label::setText(String text) {
    if (text == m_text) {
        return;
    }
    m_text = std::move(text);
    setAccessibleName(m_text);
    m_layoutWidth = -1.0f;
    invalidateLayout();
    invalidatePaint();
}

void Label::setWrap(bool wrap) {
    m_wrap = wrap;
    m_layoutWidth = -1.0f;
    invalidateLayout();
}

void Label::setMuted(bool muted) {
    m_muted = muted;
    invalidatePaint();
}

void Label::layoutText(float width) {
    const float key = m_wrap ? width : 0.0f;
    if (m_layoutWidth == key) {
        return;
    }
    m_layoutWidth = key;
    layoutIn(m_layout, m_text, theme(), m_wrap ? width : std::numeric_limits<float>::infinity(), m_wrap,
             TextAlign::Start);
}

Vec2 Label::measureContent(Vec2 available) {
    if (!theme().font) {
        return {};
    }
    m_layoutWidth = -1.0f; // the theme may have changed
    layoutText(available.x);
    return {std::ceil(m_layout.size().x), std::ceil(std::max(m_layout.size().y, theme().fontSize))};
}

void Label::paint(Painter &painter, const Theme &theme) {
    if (!theme.font || m_text.empty()) {
        return;
    }
    layoutText(rect().width);
    const Color color = !isEnabled() ? theme.textDisabled : m_muted ? theme.textMuted : theme.text;
    painter.drawText(m_layout, centredOrigin(m_layout, rect()), color);
}

// ---- Button -----------------------------------------------------------------------

Button::Button(String text) : m_text(std::move(text)) {
    setRole(Role::Button);
    setAccessibleName(m_text);
    setFocusable(true);
}

void Button::setText(String text) {
    m_text = std::move(text);
    setAccessibleName(m_text);
    m_laidOut = false;
    invalidateLayout();
    invalidatePaint();
}

void Button::setPrimary(bool primary) {
    m_primary = primary;
    invalidatePaint();
}

Vec2 Button::measureContent(Vec2) {
    const Theme &t = theme();
    m_laidOut = layoutIn(m_layout, m_text, t, std::numeric_limits<float>::infinity(), false, TextAlign::Start);
    const float textWidth = m_laidOut ? std::ceil(m_layout.size().x) : 0.0f;
    return {textWidth + 2 * t.padding * 1.5f, t.controlHeight};
}

void Button::paint(Painter &painter, const Theme &theme) {
    const bool enabled = isEnabled();
    Color fill = m_primary ? theme.accent : theme.control;
    if (!enabled) {
        fill = theme.controlDisabled;
    } else if (isPressed() && isHovered()) {
        fill = theme.controlPressed;
    } else if (isHovered()) {
        fill = m_primary ? theme.accent : theme.controlHover;
    }
    PainterPath shape;
    shape.addRoundedRect(rect(), theme.radius, theme.radius);
    painter.fillPath(shape, fill);
    if (hasFocus()) {
        PainterPath ring;
        const float inset = theme.focusRingWidth / 2.0f;
        ring.addRoundedRect(rect().grownBy(-inset, -inset, -inset, -inset), theme.radius, theme.radius);
        painter.strokePath(ring, Pen(theme.accent, theme.focusRingWidth));
    }
    if (!m_laidOut && !m_text.empty()) {
        m_laidOut = layoutIn(m_layout, m_text, theme, std::numeric_limits<float>::infinity(), false, TextAlign::Start);
    }
    if (m_laidOut) {
        const float x = std::round(rect().x + (rect().width - m_layout.size().x) / 2.0f);
        const Vec2 origin = centredOrigin(m_layout, rect());
        const Color text = !enabled ? theme.textDisabled : m_primary ? theme.accentText : theme.text;
        painter.drawText(m_layout, {x, origin.y}, text);
    }
}

bool Button::onPointer(const PointerEvent &event) {
    if (event.button != PointerButton::Left && event.type != PointerEvent::Type::Move) {
        return false;
    }
    switch (event.type) {
    case PointerEvent::Type::Press:
        return true; // becomes the pressed element
    case PointerEvent::Type::Release:
        if (isPressed() && isHovered()) {
            clicked.emit();
        }
        return true;
    default:
        return isPressed();
    }
}

bool Button::onKey(const KeyEvent &event) {
    if (event.type == KeyEvent::Type::Press && !event.repeat && (event.key == Key::Space || event.key == Key::Enter)) {
        clicked.emit();
        return true;
    }
    return false;
}

void Button::onHoverChanged(bool) { invalidatePaint(); }
void Button::onPressedChanged(bool) { invalidatePaint(); }
void Button::onFocusChanged(bool) { invalidatePaint(); }

} // namespace cfw
