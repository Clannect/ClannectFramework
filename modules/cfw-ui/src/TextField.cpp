#include <algorithm>
#include <cmath>
#include <limits>

#include "cfw/core/Utf8.h"
#include "cfw/gfx/Painter.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Theme.h"

namespace cfw {

namespace {

constexpr std::size_t kMaxUndo = 100;

std::u32string decode(StringView utf8) {
    std::u32string out;
    for (std::size_t i = 0; i < utf8.size();) {
        const Utf8Char c = decodeUtf8At(utf8, i);
        out.push_back(c.codepoint);
        i += c.length;
    }
    return out;
}

String encode(std::u32string_view text) {
    String out;
    for (const char32_t c : text) {
        appendUtf8(out, c);
    }
    return out;
}

bool isControl(char32_t c) { return c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0) || c == 0x2028 || c == 0x2029; }

bool ctrl(const KeyEvent &e) { return hasModifier(e.modifiers, Modifier::Control) || hasModifier(e.modifiers, Modifier::Meta); }

} // namespace

TextField::TextField(String text) : m_text(decode(text)) {
    setRole(Role::TextField);
    setFocusable(true);
    setCursor(Cursor::IBeam);
    m_caret = m_anchor = m_text.size();
}

void TextField::setText(StringView text) {
    std::u32string decoded = decode(text);
    if (decoded == m_text) {
        return;
    }
    m_text = std::move(decoded);
    m_caret = m_anchor = m_text.size();
    m_undo.clear();
    m_redo.clear();
    m_layoutValid = false;
    invalidatePaint();
}

String TextField::text() const { return encode(m_text); }

void TextField::setPlaceholder(String placeholder) {
    m_placeholder = std::move(placeholder);
    m_layoutValid = false;
    invalidatePaint();
}

void TextField::setSelection(std::size_t anchor, std::size_t caret) {
    m_anchor = std::min(anchor, m_text.size());
    m_caret = std::min(caret, m_text.size());
    relayout();
    invalidatePaint();
}

void TextField::selectAll() { setSelection(0, m_text.size()); }

String TextField::selectedText() const {
    const auto [from, to] = std::minmax(m_anchor, m_caret);
    return encode(std::u32string_view(m_text).substr(from, to - from));
}

Vec2 TextField::measureContent(Vec2) { return {160.0f, theme().controlHeight}; }

float TextField::textLeft() const { return rect().x + theme().padding - m_scroll; }

void TextField::relayout() {
    const Theme &t = theme();
    if (!m_layoutValid && t.font) {
        TextStyle style;
        style.font = t.font;
        style.pixelSize = t.fontSize;
        style.fallback = t.fonts.get();
        TextLayoutOptions options;
        options.wrap = false;
        m_layout.setText(Span<const char32_t>(m_text.data(), m_text.size()));
        m_layout.layout(style, options);
        m_placeholderLayout.setText(m_placeholder);
        m_placeholderLayout.layout(style, options);
        m_layoutValid = true;
    }
    // Keep the caret in view.
    const float inner = std::max(0.0f, rect().width - 2 * t.padding);
    const float caretX = t.font ? m_layout.caret(m_caret).x : 0.0f;
    if (caretX - m_scroll > inner) {
        m_scroll = caretX - inner;
    } else if (caretX < m_scroll) {
        m_scroll = caretX;
    }
    const float width = t.font ? m_layout.size().x : 0.0f;
    m_scroll = std::clamp(m_scroll, 0.0f, std::max(0.0f, width + 1.0f - inner));
}

std::size_t TextField::indexAt(float x) {
    relayout();
    if (!theme().font) {
        return m_text.size();
    }
    return std::min(m_layout.hitTest({x - textLeft(), theme().fontSize / 2.0f}), m_text.size());
}

void TextField::moveCaret(std::size_t to, bool extend) {
    m_caret = std::min(to, m_text.size());
    if (!extend) {
        m_anchor = m_caret;
    }
    relayout();
    invalidatePaint();
}

void TextField::replaceSelection(std::u32string_view with) {
    const auto [from, to] = std::minmax(m_anchor, m_caret);
    std::u32string insert(with);
    if (m_maxLength > 0) {
        const std::size_t room = m_maxLength - std::min(m_maxLength, m_text.size() - (to - from));
        insert.resize(std::min(insert.size(), room));
    }
    if (from == to && insert.empty()) {
        return;
    }
    m_undo.push_back({m_text, m_caret, m_anchor});
    if (m_undo.size() > kMaxUndo) {
        m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
    m_text.replace(from, to - from, insert);
    m_caret = m_anchor = from + insert.size();
    changed();
}

void TextField::changed() {
    m_layoutValid = false;
    m_dirty = true;
    relayout();
    invalidatePaint();
    textChanged.emit(text());
}

void TextField::paint(Painter &painter, const Theme &theme) {
    relayout();
    PainterPath shape;
    shape.addRoundedRect(rect(), theme.radius, theme.radius);
    painter.fillPath(shape, isEnabled() ? theme.panel : theme.controlDisabled);
    const bool focused = hasFocus();
    painter.strokePath(shape, Pen(focused ? theme.accent : theme.border, focused ? theme.focusRingWidth : 1.0f));
    if (!theme.font) {
        return;
    }
    const RectF inner = rect().grownBy(-theme.padding + 1, -1, -theme.padding + 1, -1);
    painter.save();
    painter.clipRect(inner);
    const TextLayout &shown = m_text.empty() ? m_placeholderLayout : m_layout;
    const auto lines = shown.lines();
    const float height = lines.empty() ? theme.fontSize : lines[0].ascent + lines[0].descent;
    const float baseline = lines.empty() ? 0.0f : lines[0].baseline;
    const float top = std::round(rect().y + (rect().height - height) / 2.0f + baseline) - baseline;
    const Vec2 origin{textLeft(), top};

    if (focused && m_anchor != m_caret) {
        const auto [from, to] = std::minmax(m_anchor, m_caret);
        const float x0 = m_layout.caret(from).x;
        const float x1 = m_layout.caret(to).x;
        Color selection = theme.accent;
        selection.a = 0.4f;
        painter.fillRect({origin.x + x0, origin.y, x1 - x0, height}, selection);
    }
    if (m_text.empty()) {
        painter.drawText(m_placeholderLayout, origin, theme.textMuted);
    } else {
        painter.drawText(m_layout, origin, isEnabled() ? theme.text : theme.textDisabled);
    }
    if (focused && !m_readOnly) {
        const float x = std::round(origin.x + m_layout.caret(m_caret).x) + 0.5f;
        painter.drawLine({x, origin.y}, {x, origin.y + height}, Pen(theme.text, 1.0f));
    }
    painter.restore();
}

bool TextField::onPointer(const PointerEvent &event) {
    switch (event.type) {
    case PointerEvent::Type::Press:
        if (event.button != PointerButton::Left) {
            return false;
        }
        if (event.clickCount >= 2) {
            selectAll();
        } else {
            moveCaret(indexAt(event.position.x), hasModifier(event.modifiers, Modifier::Shift));
        }
        return true;
    case PointerEvent::Type::Move:
        if (isPressed()) {
            moveCaret(indexAt(event.position.x), true);
            return true;
        }
        return false;
    case PointerEvent::Type::Release:
        return isPressed();
    default:
        return false;
    }
}

bool TextField::onKey(const KeyEvent &event) {
    if (event.type != KeyEvent::Type::Press) {
        return false;
    }
    const bool shift = hasModifier(event.modifiers, Modifier::Shift);
    const bool hasSelection = m_anchor != m_caret;
    const auto [from, to] = std::minmax(m_anchor, m_caret);
    switch (event.key) {
    case Key::Left:
        moveCaret(hasSelection && !shift ? from : (m_caret > 0 ? m_caret - 1 : 0), shift);
        return true;
    case Key::Right:
        moveCaret(hasSelection && !shift ? to : m_caret + 1, shift);
        return true;
    case Key::Home:
        moveCaret(0, shift);
        return true;
    case Key::End:
        moveCaret(m_text.size(), shift);
        return true;
    case Key::Backspace:
        if (m_readOnly) {
            return true;
        }
        if (!hasSelection && m_caret > 0) {
            m_anchor = m_caret - 1;
        }
        replaceSelection({});
        return true;
    case Key::Delete:
        if (m_readOnly) {
            return true;
        }
        if (!hasSelection && m_caret < m_text.size()) {
            m_anchor = m_caret + 1;
        }
        replaceSelection({});
        return true;
    case Key::Enter:
        submitted.emit(text());
        m_dirty = false;
        editingFinished.emit();
        return true;
    default:
        break;
    }
    if (!ctrl(event)) {
        return false;
    }
    Surface *s = surface();
    switch (event.key) {
    case Key::A:
        selectAll();
        return true;
    case Key::C:
        if (hasSelection && s) {
            s->setClipboardText(selectedText());
        }
        return true;
    case Key::X:
        if (hasSelection && s && !m_readOnly) {
            s->setClipboardText(selectedText());
            replaceSelection({});
        }
        return true;
    case Key::V:
        if (s && !m_readOnly) {
            std::u32string pasted = decode(s->clipboardText());
            std::erase_if(pasted, isControl); // one line: newlines and tabs are dropped
            replaceSelection(pasted);
        }
        return true;
    case Key::Z:
    case Key::Y: {
        const bool redo = event.key == Key::Y || shift;
        auto &from_ = redo ? m_redo : m_undo;
        auto &to_ = redo ? m_undo : m_redo;
        if (from_.empty() || m_readOnly) {
            return true;
        }
        to_.push_back({m_text, m_caret, m_anchor});
        const State state = std::move(from_.back());
        from_.pop_back();
        m_text = state.text;
        m_caret = state.caret;
        m_anchor = state.anchor;
        changed();
        return true;
    }
    default:
        return false;
    }
}

bool TextField::onText(const TextEvent &event) {
    if (m_readOnly) {
        return false;
    }
    std::u32string typed = decode(event.text);
    std::erase_if(typed, isControl);
    if (typed.empty()) {
        return false;
    }
    replaceSelection(typed);
    return true;
}

void TextField::onFocusChanged(bool focused) {
    if (!focused) {
        m_anchor = m_caret; // the selection goes with the focus
        if (m_dirty) {
            m_dirty = false;
            editingFinished.emit();
        }
        focusLost.emit();
    }
    invalidatePaint();
}

void TextField::onHoverChanged(bool) {}

} // namespace cfw
