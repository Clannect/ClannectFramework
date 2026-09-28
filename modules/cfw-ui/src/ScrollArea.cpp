#include <algorithm>
#include <cmath>
#include <limits>

#include "cfw/gfx/Painter.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Theme.h"

namespace cfw {

namespace {
constexpr float kMinThumb = 20.0f;
} // namespace

ScrollArea::ScrollArea() {
    setRole(Role::ScrollArea);
    setClipsChildren(true);
}

Element &ScrollArea::setContent(std::unique_ptr<Element> content) {
    if (m_content) {
        static_cast<void>(remove(*m_content));
    }
    m_content = &add(std::move(content));
    m_offset = {};
    return *m_content;
}

Vec2 ScrollArea::maxOffset() const noexcept {
    return {0.0f, std::max(0.0f, m_contentSize.y - rect().height)};
}

bool ScrollArea::showsBar() const { return m_contentSize.y > rect().height + 0.5f; }

RectF ScrollArea::viewport() const {
    const RectF r = rect();
    return showsBar() ? RectF{r.x, r.y, std::max(0.0f, r.width - kBarWidth), r.height} : r;
}

RectF ScrollArea::thumb() const {
    const RectF r = rect();
    const float thumbHeight = std::max(kMinThumb, r.height * r.height / std::max(1.0f, m_contentSize.y));
    const float range = std::max(1.0f, maxOffset().y);
    const float y = r.y + (r.height - thumbHeight) * (m_offset.y / range);
    return {r.right() - kBarWidth + 2.0f, y, kBarWidth - 4.0f, thumbHeight};
}

Vec2 ScrollArea::measureContent(Vec2 available) {
    if (!m_content) {
        return {};
    }
    const Vec2 c = m_content->measure({available.x, std::numeric_limits<float>::infinity()});
    return {c.x + kBarWidth, std::min(c.y, available.y)};
}

void ScrollArea::arrangeContent(const RectF &rect) {
    if (!m_content) {
        return;
    }
    const float inf = std::numeric_limits<float>::infinity();
    Vec2 size = m_content->measure({rect.width, inf});
    if (size.y > rect.height + 0.5f) {
        size = m_content->measure({std::max(0.0f, rect.width - kBarWidth), inf}); // make room for the bar
    }
    m_contentSize = size;
    m_offset.y = std::clamp(m_offset.y, 0.0f, maxOffset().y);
    const RectF view = viewport();
    m_content->arrange({view.x, std::round(view.y - m_offset.y), view.width, size.y});
}

void ScrollArea::scrollTo(Vec2 offset) {
    const Vec2 clamped{0.0f, std::clamp(offset.y, 0.0f, maxOffset().y)};
    if (clamped == m_offset) {
        return;
    }
    m_offset = clamped;
    arrangeContent(rect());
    invalidatePaint();
    scrolled.emit(m_offset);
}

void ScrollArea::ensureVisible(const RectF &target) {
    const RectF view = viewport();
    if (target.y < view.y) {
        scrollTo({0.0f, m_offset.y - (view.y - target.y)});
    } else if (target.bottom() > view.bottom()) {
        scrollTo({0.0f, m_offset.y + std::min(target.bottom() - view.bottom(), target.y - view.y)});
    }
}

void ScrollArea::paint(Painter &painter, const Theme &theme) {
    if (!showsBar()) {
        return;
    }
    const RectF r = rect();
    Color track = theme.panel;
    painter.fillRect({r.right() - kBarWidth, r.y, kBarWidth, r.height}, track);
    PainterPath shape;
    const RectF t = thumb();
    shape.addRoundedRect(t, t.width / 2.0f, t.width / 2.0f);
    painter.fillPath(shape, m_dragFrom ? theme.textMuted : theme.border);
}

bool ScrollArea::onPointer(const PointerEvent &event) {
    const RectF r = rect();
    switch (event.type) {
    case PointerEvent::Type::Wheel: {
        const Vec2 before = m_offset;
        scrollTo({0.0f, m_offset.y - event.wheelDelta.y});
        return !(before == m_offset); // at an end, let an outer area scroll
    }
    case PointerEvent::Type::Press: {
        if (event.button != PointerButton::Left || !showsBar() || event.position.x < r.right() - kBarWidth) {
            return false;
        }
        const RectF t = thumb();
        if (event.position.y >= t.y && event.position.y < t.bottom()) {
            m_dragFrom = event.position.y - t.y;
        } else {
            // Clicking the track pages toward the pointer.
            const float page = r.height * 0.9f;
            scrollTo({0.0f, m_offset.y + (event.position.y < t.y ? -page : page)});
        }
        invalidatePaint();
        return true;
    }
    case PointerEvent::Type::Move:
        if (m_dragFrom && isPressed()) {
            const float thumbHeight = thumb().height;
            const float travel = std::max(1.0f, r.height - thumbHeight);
            const float top = event.position.y - *m_dragFrom - r.y;
            scrollTo({0.0f, top / travel * maxOffset().y});
            return true;
        }
        return false;
    case PointerEvent::Type::Release:
        if (m_dragFrom) {
            m_dragFrom.reset();
            invalidatePaint();
            return true;
        }
        return false;
    default:
        return false;
    }
}

bool ScrollArea::onKey(const KeyEvent &event) {
    if (event.type != KeyEvent::Type::Press) {
        return false;
    }
    const float page = rect().height * 0.9f;
    switch (event.key) {
    case Key::PageUp: scrollTo({0.0f, m_offset.y - page}); return true;
    case Key::PageDown: scrollTo({0.0f, m_offset.y + page}); return true;
    case Key::Home:
        if (hasModifier(event.modifiers, Modifier::Control)) {
            scrollTo({});
            return true;
        }
        return false;
    case Key::End:
        if (hasModifier(event.modifiers, Modifier::Control)) {
            scrollTo(maxOffset());
            return true;
        }
        return false;
    default:
        return false;
    }
}

} // namespace cfw
