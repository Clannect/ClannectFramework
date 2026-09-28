// Panel, Separator, ToolBar, MenuBar, Slider and ProgressBar.

#include "cfw/ui/Chrome.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "ControlText.h"
#include "cfw/gfx/Painter.h"
#include "cfw/ui/Surface.h"

namespace cfw {

using detail::centredOrigin;
using detail::layoutIn;

namespace {

bool inside(const RectF &r, Vec2 p) { return p.x >= r.x && p.y >= r.y && p.x < r.right() && p.y < r.bottom(); }

// "&File" -> "File": menus mark mnemonics the way Qt does.
String withoutMnemonic(StringView text) {
    String out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '&' && i + 1 < text.size()) {
            ++i; // "&&" is a literal ampersand
        }
        out += text[i];
    }
    return out;
}

} // namespace

// ---- Panel and Separator ---------------------------------------------------------

Panel::Panel(Direction direction, float spacing, float padding) : Stack(direction, spacing, padding) {}

void Panel::setBorder(Border border) {
    m_border = border;
    invalidatePaint();
}

void Panel::setRecessed(bool recessed) {
    m_recessed = recessed;
    invalidatePaint();
}

void Panel::paint(Painter &painter, const Theme &theme) {
    const RectF r = rect();
    painter.fillRect(r, m_recessed ? theme.window : theme.panel);
    const Pen line(theme.border, 1.0f);
    const float top = r.y + 0.5f;
    const float bottom = r.bottom() - 0.5f;
    const float left = r.x + 0.5f;
    const float right = r.right() - 0.5f;
    switch (m_border) {
    case Border::None: break;
    case Border::Top: painter.drawLine({r.x, top}, {r.right(), top}, line); break;
    case Border::Bottom: painter.drawLine({r.x, bottom}, {r.right(), bottom}, line); break;
    case Border::Left: painter.drawLine({left, r.y}, {left, r.bottom()}, line); break;
    case Border::Right: painter.drawLine({right, r.y}, {right, r.bottom()}, line); break;
    case Border::All: painter.strokeRect({left, top, r.width - 1.0f, r.height - 1.0f}, line); break;
    }
}

Separator::Separator(Stack::Direction along) : m_along(along) { setRole(Role::Separator); }

Vec2 Separator::measureContent(Vec2) {
    return m_along == Stack::Direction::Row ? Vec2{1.0f, 0.0f} : Vec2{0.0f, 1.0f};
}

void Separator::paint(Painter &painter, const Theme &theme) {
    const RectF r = rect();
    if (m_along == Stack::Direction::Row) {
        const float x = std::round(r.x + r.width / 2.0f - 0.5f) + 0.5f;
        painter.drawLine({x, r.y + 4.0f}, {x, r.bottom() - 4.0f}, Pen(theme.border, 1.0f));
    } else {
        const float y = std::round(r.y + r.height / 2.0f - 0.5f) + 0.5f;
        painter.drawLine({r.x, y}, {r.right(), y}, Pen(theme.border, 1.0f));
    }
}

// ---- ToolBar -----------------------------------------------------------------------

ToolBar::ToolBar() : Panel(Direction::Row, 2.0f, 4.0f) {
    setBorder(Border::Bottom);
    setRole(Role::ToolBar);
}

Button &ToolBar::addButton(Icon icon, String toolTip, std::function<void()> action) {
    Button &button = add<Button>(std::move(icon));
    button.setFlat(true);
    button.setAccessibleName(toolTip);
    button.setToolTip(std::move(toolTip));
    button.setFocusable(false); // toolbars do not take the keyboard from the content
    if (action) {
        static_cast<void>(button.clicked.connect(std::move(action)));
    }
    return button;
}

Button &ToolBar::addToggle(Icon icon, String toolTip, std::function<void(bool)> action, bool checked) {
    Button &button = addButton(std::move(icon), std::move(toolTip), {});
    button.setCheckable(true);
    button.setChecked(checked);
    static_cast<void>(button.toggled.connect(std::move(action)));
    return button;
}

void ToolBar::addSeparator() { add<Separator>(Stack::Direction::Row); }

void ToolBar::addSpacer() { add<Element>().setStretch(1.0f); }

// ---- MenuBar -----------------------------------------------------------------------

MenuBar::MenuBar() : Panel(Direction::Row, 0.0f, 0.0f) {
    setBorder(Border::Bottom);
    setRole(Role::MenuBar);
}

void MenuBar::addMenu(String title, std::function<void(Menu &)> fill) {
    m_titles.push_back({withoutMnemonic(title), std::move(fill), {}, {}});
    invalidateLayout();
    invalidatePaint();
}

Vec2 MenuBar::measureContent(Vec2) {
    const Theme &t = theme();
    float width = 0.0f;
    for (Title &title : m_titles) {
        if (layoutIn(title.layout, title.text, t, std::numeric_limits<float>::infinity(), false, TextAlign::Start)) {
            width += std::ceil(title.layout.size().x) + 2 * t.padding;
        }
    }
    return {width, t.controlHeight};
}

void MenuBar::layoutTitles() {
    const Theme &t = theme();
    float x = rect().x + 4.0f;
    for (Title &title : m_titles) {
        const float w = (t.font ? std::ceil(title.layout.size().x) : 0.0f) + 2 * t.padding;
        title.rect = {x, rect().y, w, rect().height - 1.0f};
        x += w;
    }
}

std::ptrdiff_t MenuBar::titleAt(Vec2 point) const {
    for (std::size_t i = 0; i < m_titles.size(); ++i) {
        if (inside(m_titles[i].rect, point)) {
            return std::ptrdiff_t(i);
        }
    }
    return -1;
}

void MenuBar::paint(Painter &painter, const Theme &theme) {
    Panel::paint(painter, theme);
    layoutTitles();
    for (std::size_t i = 0; i < m_titles.size(); ++i) {
        const Title &title = m_titles[i];
        const bool open = std::ptrdiff_t(i) == m_open;
        const bool hot = std::ptrdiff_t(i) == m_hot && isHovered();
        if (open || hot) {
            PainterPath shape;
            shape.addRoundedRect(title.rect.grownBy(-1, -3, -1, -3), theme.radius, theme.radius);
            painter.fillPath(shape, open ? theme.controlPressed : theme.controlHover);
        }
        if (theme.font) {
            const Vec2 origin = centredOrigin(title.layout, title.rect);
            painter.drawText(title.layout, {title.rect.x + theme.padding, origin.y},
                             isEnabled() ? theme.text : theme.textDisabled);
        }
    }
}

void MenuBar::openMenu(std::size_t index, bool fromKeyboard) {
    Surface *s = surface();
    if (!s || index >= m_titles.size()) {
        return;
    }
    if (m_menu) {
        Menu *old = m_menu;
        m_menu = nullptr; // so its onClosed does not clear the new state
        s->closePopup(*old);
    }
    layoutTitles();
    auto menu = std::make_unique<Menu>();
    m_titles[index].fill(*menu);
    menu->sideways = [this](int direction) {
        const std::size_t count = m_titles.size();
        if (count == 0 || m_open < 0) {
            return;
        }
        openMenu((std::size_t(m_open) + count + std::size_t(direction + int(count))) % count, true);
    };
    Menu *opened = menu.get();
    s->openPopup(std::move(menu), {m_titles[index].rect.x, rect().bottom()}, [this, opened] {
        if (m_menu == opened) {
            m_menu = nullptr;
            m_open = -1;
            invalidatePaint();
        }
    });
    m_menu = opened;
    m_open = std::ptrdiff_t(index);
    invalidatePaint();
    if (fromKeyboard) {
        s->layout();
        opened->focusFirstItem();
    }
}

bool MenuBar::onPointer(const PointerEvent &event) {
    layoutTitles();
    const std::ptrdiff_t at = titleAt(event.position);
    switch (event.type) {
    case PointerEvent::Type::Move:
        if (at != m_hot) {
            m_hot = at;
            invalidatePaint();
        }
        // With a menu open, the pointer switches between titles.
        if (m_menu && at >= 0 && at != m_open) {
            openMenu(std::size_t(at));
        }
        return false;
    case PointerEvent::Type::Press:
        if (event.button != PointerButton::Left || at < 0) {
            return false;
        }
        if (at == m_open && m_menu) {
            if (Surface *s = surface()) {
                s->closePopup(*m_menu);
            }
        } else {
            openMenu(std::size_t(at));
        }
        return true;
    case PointerEvent::Type::Leave:
        m_hot = -1;
        invalidatePaint();
        return false;
    default:
        return false;
    }
}

// ---- Slider ------------------------------------------------------------------------

Slider::Slider(double minimum, double maximum, double value)
    : m_minimum(std::min(minimum, maximum)), m_maximum(std::max(minimum, maximum)),
      m_value(std::clamp(value, m_minimum, m_maximum)) {
    setRole(Role::Slider);
    setAccessibleName("slider");
    setFocusable(true);
}

void Slider::setRange(double minimum, double maximum) {
    m_minimum = std::min(minimum, maximum);
    m_maximum = std::max(minimum, maximum);
    m_value = std::clamp(m_value, m_minimum, m_maximum);
    invalidatePaint();
}

void Slider::setValue(double value) {
    value = std::clamp(value, m_minimum, m_maximum);
    if (value != m_value) {
        m_value = value;
        invalidatePaint();
    }
}

void Slider::change(double value) {
    value = std::clamp(value, m_minimum, m_maximum);
    if (value != m_value) {
        m_value = value;
        invalidatePaint();
        valueChanged.emit(m_value);
    }
}

Vec2 Slider::measureContent(Vec2) { return {120.0f, theme().controlHeight}; }

RectF Slider::track() const {
    constexpr float kThumb = 7.0f; // radius: the track ends where the thumb's centre can go
    const RectF r = rect();
    return {r.x + kThumb, r.y + r.height / 2.0f - 2.0f, std::max(0.0f, r.width - 2 * kThumb), 4.0f};
}

double Slider::valueAt(float x) const {
    const RectF t = track();
    const double fraction = t.width > 0 ? std::clamp(double(x - t.x) / double(t.width), 0.0, 1.0) : 0.0;
    return m_minimum + fraction * (m_maximum - m_minimum);
}

void Slider::paint(Painter &painter, const Theme &theme) {
    const RectF t = track();
    const bool enabled = isEnabled();
    PainterPath groove;
    groove.addRoundedRect(t, 2.0f, 2.0f);
    painter.fillPath(groove, theme.control);
    const double span = m_maximum - m_minimum;
    const float fraction = span > 0 ? float((m_value - m_minimum) / span) : 0.0f;
    const float cx = t.x + t.width * fraction;
    PainterPath filled;
    filled.addRoundedRect({t.x, t.y, cx - t.x, t.height}, 2.0f, 2.0f);
    painter.fillPath(filled, enabled ? theme.accent : theme.textDisabled);
    PainterPath thumb;
    const float radius = isHovered() || isPressed() ? 7.0f : 6.0f;
    const float cy = t.y + t.height / 2.0f;
    thumb.addEllipse({cx - radius, cy - radius, radius * 2, radius * 2});
    painter.fillPath(thumb, enabled ? theme.text : theme.textDisabled);
    if (hasFocus()) {
        PainterPath ring;
        ring.addEllipse({cx - radius - 2, cy - radius - 2, radius * 2 + 4, radius * 2 + 4});
        painter.strokePath(ring, Pen(theme.accent, theme.focusRingWidth));
    }
}

bool Slider::onPointer(const PointerEvent &event) {
    switch (event.type) {
    case PointerEvent::Type::Press:
        if (event.button != PointerButton::Left) {
            return false;
        }
        change(valueAt(event.position.x));
        return true;
    case PointerEvent::Type::Move:
        if (isPressed()) {
            change(valueAt(event.position.x));
            return true;
        }
        return false;
    case PointerEvent::Type::Release:
        return isPressed();
    case PointerEvent::Type::Wheel: {
        const double step = m_step > 0 ? m_step : (m_maximum - m_minimum) / 100.0;
        change(m_value + (event.wheelDelta.y > 0 ? step : -step));
        return true;
    }
    default:
        return false;
    }
}

bool Slider::onKey(const KeyEvent &event) {
    if (event.type != KeyEvent::Type::Press) {
        return false;
    }
    const double step = (m_step > 0 ? m_step : (m_maximum - m_minimum) / 100.0) *
                        (hasModifier(event.modifiers, Modifier::Shift) ? 10.0 : 1.0);
    switch (event.key) {
    case Key::Left:
    case Key::Down: change(m_value - step); return true;
    case Key::Right:
    case Key::Up: change(m_value + step); return true;
    case Key::Home: change(m_minimum); return true;
    case Key::End: change(m_maximum); return true;
    default: return false;
    }
}

// ---- ProgressBar ---------------------------------------------------------------------

ProgressBar::ProgressBar() {
    setRole(Role::ProgressBar);
    setAccessibleName("progress");
}

ProgressBar::~ProgressBar() { stopAnimation(); }

void ProgressBar::stopAnimation() {
    if (m_timer && m_timerSurface) {
        m_timerSurface->stopTimer(m_timer);
    }
    m_timer = 0;
    m_timerSurface = nullptr;
}

void ProgressBar::setValue(float fraction) {
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (fraction != m_value) {
        m_value = fraction;
        invalidatePaint();
    }
}

void ProgressBar::setBusy(bool busy) {
    if (busy == m_busy) {
        return;
    }
    m_busy = busy;
    if (!busy) {
        stopAnimation();
    }
    invalidatePaint();
}

Vec2 ProgressBar::measureContent(Vec2) { return {120.0f, 6.0f}; }

void ProgressBar::paint(Painter &painter, const Theme &theme) {
    const RectF r = rect();
    PainterPath groove;
    groove.addRoundedRect(r, r.height / 2.0f, r.height / 2.0f);
    painter.fillPath(groove, theme.control);
    painter.save();
    painter.clipPath(groove);
    if (m_busy) {
        // A third of the bar sweeping across, advanced by a surface timer
        // that runs only while the bar is painted.
        const float w = r.width / 3.0f;
        const float x = r.x - w + (r.width + w) * m_phase;
        painter.fillRect({x, r.y, w, r.height}, theme.accent);
        Surface *s = surface();
        if (s && !m_timer) {
            m_timerSurface = s;
            m_timer = s->startTimer(
                std::chrono::milliseconds(16),
                [this] {
                    if (surface() != m_timerSurface || !m_busy) {
                        stopAnimation();
                        return;
                    }
                    m_phase = std::fmod(m_phase + 0.012f, 1.0f);
                    invalidatePaint();
                },
                true);
        }
    } else {
        painter.fillRect({r.x, r.y, r.width * m_value, r.height}, theme.accent);
    }
    painter.restore();
}

} // namespace cfw
