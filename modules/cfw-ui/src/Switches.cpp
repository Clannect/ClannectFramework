#include "cfw/ui/Switches.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "cfw/gfx/Painter.h"
#include "cfw/ui/Theme.h"

namespace cfw {

namespace {

constexpr auto kAnimation = std::chrono::milliseconds(150);
constexpr auto kFrame = std::chrono::milliseconds(16);

// 0..1 along the animation, eased out.
float progress(TimePoint start, TimePoint now) {
    const float t = std::chrono::duration<float>(now - start).count() /
                    std::chrono::duration<float>(kAnimation).count();
    const float c = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - c) * (1.0f - c);
}

Color mix(Color a, Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

} // namespace

// ---- ToggleSwitch -------------------------------------------------------------------

ToggleSwitch::ToggleSwitch(bool checked, String accessibleName) : m_checked(checked), m_position(checked ? 1.0f : 0.0f) {
    setRole(Role::CheckBox);
    setAccessibleName(std::move(accessibleName));
    setFocusable(true);
    setCursor(Cursor::Hand);
}

ToggleSwitch::~ToggleSwitch() {
    if (m_timer) {
        if (Surface *s = surface()) {
            s->stopTimer(m_timer);
        }
    }
}

void ToggleSwitch::setChecked(bool checked) {
    if (checked == m_checked) {
        return;
    }
    m_checked = checked;
    animateTo(checked ? 1.0f : 0.0f);
}

void ToggleSwitch::toggle() {
    m_checked = !m_checked;
    animateTo(m_checked ? 1.0f : 0.0f);
    toggled.emit(m_checked);
}

void ToggleSwitch::animateTo(float target) {
    Surface *s = surface();
    if (!s || theme().reducedMotion) {
        if (m_timer && s) {
            s->stopTimer(m_timer);
        }
        m_timer = 0;
        m_position = target;
        invalidatePaint();
        return;
    }
    m_from = m_position;
    m_animStart = s->now();
    if (!m_timer) {
        m_timer = s->startTimer(
            kFrame,
            [this] {
                Surface *surf = surface();
                if (!surf) {
                    return;
                }
                const float end = m_checked ? 1.0f : 0.0f;
                const float t = progress(m_animStart, surf->now());
                m_position = m_from + (end - m_from) * t;
                invalidatePaint();
                if (t >= 1.0f) {
                    surf->stopTimer(m_timer);
                    m_timer = 0;
                }
            },
            true);
    }
}

Vec2 ToggleSwitch::measureContent(Vec2) { return {34.0f, 20.0f}; }

void ToggleSwitch::paint(Painter &painter, const Theme &theme) {
    const RectF r{rect().x, std::round(rect().y + (rect().height - 20.0f) / 2.0f), 34.0f, 20.0f};
    const bool enabled = isEnabled();
    const Color off = isHovered() ? theme.controlRaised : theme.controlHover;
    PainterPath track;
    track.addRoundedRect(r, 10.0f, 10.0f);
    painter.fillPath(track, enabled ? mix(off, theme.accent, m_position) : theme.controlDisabled);
    PainterPath outline;
    outline.addRoundedRect(r.grownBy(-0.5f, -0.5f, -0.5f, -0.5f), 9.5f, 9.5f);
    painter.strokePath(outline, Pen(mix(theme.border, theme.accent, m_position), 1.0f));
    const float x = r.x + 3.0f + 14.0f * m_position;
    PainterPath knob;
    knob.addEllipse({x, r.y + 3.0f, 14.0f, 14.0f});
    painter.fillPath(knob, enabled ? mix(theme.textMuted, Color{1, 1, 1, 1}, m_position) : theme.textDisabled);
}

void ToggleSwitch::paintOverlay(Painter &painter, const Theme &theme) {
    const Surface *s = surface();
    if (!hasFocus() || !s || !s->focusVisible()) {
        return;
    }
    const RectF r{rect().x - 2.0f, std::round(rect().y + (rect().height - 20.0f) / 2.0f) - 2.0f, 38.0f, 24.0f};
    PainterPath ring;
    ring.addRoundedRect(r, 12.0f, 12.0f);
    painter.strokePath(ring, Pen(theme.accent, theme.focusRingWidth));
}

bool ToggleSwitch::onPointer(const PointerEvent &event) {
    if (event.button != PointerButton::Left && event.type != PointerEvent::Type::Move) {
        return false;
    }
    switch (event.type) {
    case PointerEvent::Type::Press: return true;
    case PointerEvent::Type::Release:
        if (isPressed() && isHovered()) {
            toggle();
        }
        return true;
    default: return isPressed();
    }
}

bool ToggleSwitch::onKey(const KeyEvent &event) {
    if (event.type == KeyEvent::Type::Press && !event.repeat && (event.key == Key::Space || event.key == Key::Enter)) {
        toggle();
        return true;
    }
    return false;
}

void ToggleSwitch::describeAccessible(AccessibleNode &node) const {
    Element::describeAccessible(node);
    node.states.checkable = true;
    node.states.checked = m_checked;
}

bool ToggleSwitch::accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) {
    if (action == AccessibleAction::Default && !item) {
        toggle();
        return true;
    }
    return Element::accessibleAction(action, item, value);
}

// ---- SegmentedControl ---------------------------------------------------------------

SegmentedControl::SegmentedControl(std::vector<String> items, int current)
    : m_items(std::move(items)), m_current(current) {
    setRole(Role::TabList);
    setFocusable(true);
    setCursor(Cursor::Hand);
}

SegmentedControl::~SegmentedControl() {
    if (m_timer) {
        if (Surface *s = surface()) {
            s->stopTimer(m_timer);
        }
    }
}

void SegmentedControl::setItems(std::vector<String> items) {
    m_items = std::move(items);
    m_layoutsValid = false;
    m_current = std::clamp(m_current, 0, std::max(0, int(m_items.size()) - 1));
    invalidateLayout();
    invalidatePaint();
}

void SegmentedControl::setCurrentIndex(int index) {
    if (index < 0 || index >= int(m_items.size()) || index == m_current) {
        return;
    }
    m_current = index;
    m_highlightX = -1.0f; // jump: not a user's choice
    invalidatePaint();
}

void SegmentedControl::setFill(bool fill) {
    m_fill = fill;
    invalidateLayout();
}

void SegmentedControl::setSegmentHeight(float height) {
    m_height = height;
    invalidateLayout();
}

void SegmentedControl::setPixelSize(float size) {
    m_pixelSize = size;
    m_layoutsValid = false;
    invalidateLayout();
}

float SegmentedControl::pixelSize() const { return m_pixelSize > 0.0f ? m_pixelSize : theme().fontSize - 1.0f; }

void SegmentedControl::relayoutText() {
    if (m_layoutsValid) {
        return;
    }
    const Theme &t = theme();
    m_layouts.clear();
    m_layouts.resize(m_items.size());
    TextStyle style;
    style.font = t.strongFont ? t.strongFont : t.font;
    style.pixelSize = pixelSize();
    style.fallback = t.fonts.get();
    TextLayoutOptions options;
    options.wrap = false;
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        if (style.font) {
            m_layouts[i].setText(m_items[i]);
            m_layouts[i].layout(style, options);
        }
    }
    m_layoutsValid = true;
}

Vec2 SegmentedControl::measureContent(Vec2 available) {
    m_layoutsValid = false;
    relayoutText();
    float width = kInset * 2.0f + kGap * float(std::max<std::size_t>(1, m_items.size()) - 1);
    for (const TextLayout &l : m_layouts) {
        width += std::ceil(l.size().x) + kTextPadding * 2.0f;
    }
    if (m_fill && std::isfinite(available.x)) {
        width = std::max(width, available.x);
    }
    return {width, m_height + kInset * 2.0f};
}

RectF SegmentedControl::segmentRect(int index) const {
    if (index < 0 || index >= int(m_items.size())) {
        return {};
    }
    const RectF r = rect();
    const float inner = r.width - kInset * 2.0f - kGap * float(m_items.size() - 1);
    float x = r.x + kInset;
    for (int i = 0; i <= index; ++i) {
        float w = 0.0f;
        if (m_fill) {
            w = inner / float(m_items.size());
        } else {
            w = (std::size_t(i) < m_layouts.size() ? std::ceil(m_layouts[std::size_t(i)].size().x) : 0.0f) +
                kTextPadding * 2.0f;
        }
        if (i == index) {
            return {std::round(x), r.y + kInset, std::round(x + w) - std::round(x), r.height - kInset * 2.0f};
        }
        x += w + kGap;
    }
    return {};
}

int SegmentedControl::segmentAt(Vec2 position) const {
    for (int i = 0; i < int(m_items.size()); ++i) {
        const RectF s = segmentRect(i);
        if (position.x >= s.x && position.x < s.x + s.width && position.y >= rect().y &&
            position.y < rect().y + rect().height) {
            return i;
        }
    }
    return -1;
}

void SegmentedControl::paint(Painter &painter, const Theme &theme) {
    relayoutText();
    const RectF r = rect();
    PainterPath track;
    track.addRoundedRect(r, theme.radiusLarge, theme.radiusLarge);
    painter.fillPath(track, theme.panel);
    PainterPath outline;
    outline.addRoundedRect(r.grownBy(-0.5f, -0.5f, -0.5f, -0.5f), theme.radiusLarge - 0.5f, theme.radiusLarge - 0.5f);
    painter.strokePath(outline, Pen(theme.border, 1.0f));
    if (m_current >= 0 && m_current < int(m_items.size())) {
        RectF chosen = segmentRect(m_current);
        if (m_highlightX >= 0.0f) {
            chosen.x = m_highlightX;
        }
        PainterPath raised;
        const float radius = std::max(2.0f, theme.radiusLarge - 2.0f);
        raised.addRoundedRect(chosen, radius, radius);
        painter.fillPath(raised, theme.controlRaised);
    }
    for (int i = 0; i < int(m_items.size()) && std::size_t(i) < m_layouts.size(); ++i) {
        const RectF s = segmentRect(i);
        const TextLayout &l = m_layouts[std::size_t(i)];
        const bool on = i == m_current;
        const Color ink = !isEnabled() ? theme.textDisabled : on || i == m_hoveredSegment ? theme.text : theme.textMuted;
        const auto lines = l.lines();
        if (lines.empty()) {
            continue;
        }
        const float height = lines[0].ascent + lines[0].descent;
        const float x = std::round(s.x + (s.width - l.size().x) / 2.0f);
        const float top = s.y + (s.height - height) / 2.0f;
        painter.drawText(l, {x, std::round(top + lines[0].baseline) - lines[0].baseline}, ink);
    }
}

void SegmentedControl::paintOverlay(Painter &painter, const Theme &theme) {
    const Surface *s = surface();
    if (!hasFocus() || !s || !s->focusVisible()) {
        return;
    }
    PainterPath ring;
    ring.addRoundedRect(segmentRect(m_current), theme.radius, theme.radius);
    painter.strokePath(ring, Pen(theme.accent, theme.focusRingWidth));
}

void SegmentedControl::choose(int index) {
    if (index < 0 || index >= int(m_items.size()) || index == m_current) {
        return;
    }
    Surface *s = surface();
    const RectF from = segmentRect(m_current);
    m_current = index;
    if (s && !theme().reducedMotion) {
        m_highlightFrom = m_highlightX >= 0.0f ? m_highlightX : from.x;
        m_highlightTo = segmentRect(index).x;
        m_highlightX = m_highlightFrom;
        m_animStart = s->now();
        if (!m_timer) {
            m_timer = s->startTimer(
                kFrame,
                [this] {
                    Surface *surf = surface();
                    if (!surf) {
                        return;
                    }
                    const float t = progress(m_animStart, surf->now());
                    m_highlightTo = segmentRect(m_current).x; // follows a resize
                    m_highlightX = m_highlightFrom + (m_highlightTo - m_highlightFrom) * t;
                    if (t >= 1.0f) {
                        m_highlightX = -1.0f;
                        surf->stopTimer(m_timer);
                        m_timer = 0;
                    }
                    invalidatePaint();
                },
                true);
        }
    } else {
        m_highlightX = -1.0f;
    }
    invalidatePaint();
    currentChanged.emit(index);
}

bool SegmentedControl::onPointer(const PointerEvent &event) {
    if (event.type == PointerEvent::Type::Move || event.type == PointerEvent::Type::Leave) {
        const int over = event.type == PointerEvent::Type::Leave ? -1 : segmentAt(event.position);
        if (over != m_hoveredSegment) {
            m_hoveredSegment = over;
            invalidatePaint();
        }
        return isPressed();
    }
    if (event.button != PointerButton::Left) {
        return false;
    }
    if (event.type == PointerEvent::Type::Press) {
        return true;
    }
    if (event.type == PointerEvent::Type::Release && isPressed() && isHovered()) {
        choose(segmentAt(event.position));
    }
    return true;
}

bool SegmentedControl::onKey(const KeyEvent &event) {
    if (event.type != KeyEvent::Type::Press || m_items.empty()) {
        return false;
    }
    switch (event.key) {
    case Key::Left: choose(std::max(0, m_current - 1)); return true;
    case Key::Right: choose(std::min(int(m_items.size()) - 1, m_current + 1)); return true;
    case Key::Home: choose(0); return true;
    case Key::End: choose(int(m_items.size()) - 1); return true;
    default: return false;
    }
}

void SegmentedControl::describeAccessible(AccessibleNode &node) const { Element::describeAccessible(node); }

void SegmentedControl::accessibleItems(std::vector<AccessibleNode> &items) const {
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        AccessibleNode seg;
        seg.itemKey = i;
        seg.role = Role::Tab;
        seg.name = m_items[i];
        seg.bounds = segmentRect(int(i));
        seg.states.selectable = true;
        seg.states.selected = int(i) == m_current;
        seg.states.focusable = true;
        seg.states.focused = hasFocus() && int(i) == m_current;
        items.push_back(std::move(seg));
    }
}

std::optional<std::uint64_t> SegmentedControl::accessibleFocusedItem() const {
    if (m_current < 0) {
        return std::nullopt;
    }
    return std::uint64_t(m_current);
}

bool SegmentedControl::accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) {
    if (item && (action == AccessibleAction::Select || action == AccessibleAction::Default)) {
        choose(int(*item));
        return true;
    }
    return Element::accessibleAction(action, item, value);
}

} // namespace cfw
