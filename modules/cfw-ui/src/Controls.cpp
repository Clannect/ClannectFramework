#include "cfw/ui/Controls.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "cfw/gfx/Painter.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Theme.h"
#include "ControlText.h"

namespace cfw {

using detail::centredOrigin;
using detail::layoutIn;



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

Button::Button(Icon icon, String text) : Button(std::move(text)) { m_icon = std::move(icon); }

void Button::setText(String text) {
    m_text = std::move(text);
    if (!m_text.empty() || accessibleName().empty()) {
        setAccessibleName(m_text);
    }
    m_laidOut = false;
    invalidateLayout();
    invalidatePaint();
}

void Button::setIcon(Icon icon) {
    m_icon = std::move(icon);
    invalidateLayout();
    invalidatePaint();
}

void Button::setIconSize(float size) {
    m_iconSize = size;
    invalidateLayout();
    invalidatePaint();
}

void Button::setPrimary(bool primary) {
    m_primary = primary;
    invalidatePaint();
}

void Button::setFlat(bool flat) {
    m_flat = flat;
    invalidatePaint();
}

void Button::setChecked(bool checked) {
    if (checked != m_checked) {
        m_checked = checked;
        invalidatePaint();
    }
}

Vec2 Button::measureContent(Vec2) {
    const Theme &t = theme();
    m_laidOut = !m_text.empty() &&
                layoutIn(m_layout, m_text, t, std::numeric_limits<float>::infinity(), false, TextAlign::Start);
    const float textWidth = m_laidOut ? std::ceil(m_layout.size().x) : 0.0f;
    if (m_text.empty() && !m_icon.isNull()) {
        return {t.controlHeight, t.controlHeight}; // a square icon button
    }
    const float iconWidth = m_icon.isNull() ? 0.0f : m_iconSize + t.spacing;
    return {iconWidth + textWidth + 2 * t.padding * 1.5f, t.controlHeight};
}

void Button::paint(Painter &painter, const Theme &theme) {
    const bool enabled = isEnabled();
    std::optional<Color> fill = m_primary ? theme.accent : theme.control;
    if (m_flat && !m_primary) {
        fill.reset();
    }
    if (!enabled) {
        fill = m_flat ? std::nullopt : std::optional<Color>(theme.controlDisabled);
    } else if (isPressed() && isHovered()) {
        fill = theme.controlPressed;
    } else if (isHovered()) {
        fill = m_primary ? theme.accent : theme.controlHover;
    } else if (m_checked) {
        fill = theme.controlPressed;
    }
    PainterPath shape;
    shape.addRoundedRect(rect(), theme.radius, theme.radius);
    if (fill) {
        painter.fillPath(shape, *fill);
    }
    if (m_checked) {
        // A checked tool shows an accent outline, whatever its fill.
        PainterPath ring;
        ring.addRoundedRect(rect().grownBy(-0.5f, -0.5f, -0.5f, -0.5f), theme.radius, theme.radius);
        painter.strokePath(ring, Pen(theme.accent, 1.0f));
    }
    if (hasFocus()) {
        PainterPath ring;
        const float inset = theme.focusRingWidth / 2.0f;
        ring.addRoundedRect(rect().grownBy(-inset, -inset, -inset, -inset), theme.radius, theme.radius);
        painter.strokePath(ring, Pen(theme.accent, theme.focusRingWidth));
    }
    if (!m_laidOut && !m_text.empty()) {
        m_laidOut = layoutIn(m_layout, m_text, theme, std::numeric_limits<float>::infinity(), false, TextAlign::Start);
    }
    const Color ink = !enabled   ? theme.textDisabled
                      : m_primary ? theme.accentText
                      : m_checked ? theme.accent
                                  : theme.text;
    const float textWidth = m_laidOut ? m_layout.size().x : 0.0f;
    const float iconWidth = m_icon.isNull() ? 0.0f : m_iconSize + (m_laidOut ? theme.spacing : 0.0f);
    float x = std::round(rect().x + (rect().width - iconWidth - textWidth) / 2.0f);
    if (!m_icon.isNull()) {
        const float y = std::round(rect().y + (rect().height - m_iconSize) / 2.0f);
        m_icon.paint(painter, {x, y, m_iconSize, m_iconSize}, ink);
        x += iconWidth;
    }
    if (m_laidOut) {
        const Vec2 origin = centredOrigin(m_layout, rect());
        painter.drawText(m_layout, {x, origin.y}, m_checked && !m_primary && enabled ? theme.text : ink);
    }
}

void Button::activate() {
    if (m_checkable) {
        m_checked = !m_checked;
        invalidatePaint();
        toggled.emit(m_checked);
    }
    clicked.emit();
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
            activate();
        }
        return true;
    default:
        return isPressed();
    }
}

bool Button::onKey(const KeyEvent &event) {
    if (event.type == KeyEvent::Type::Press && !event.repeat && (event.key == Key::Space || event.key == Key::Enter)) {
        activate();
        return true;
    }
    return false;
}

void Button::onHoverChanged(bool) { invalidatePaint(); }
void Button::onPressedChanged(bool) { invalidatePaint(); }
void Button::onFocusChanged(bool) { invalidatePaint(); }

} // namespace cfw
