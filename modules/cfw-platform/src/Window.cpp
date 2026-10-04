#include "cfw/platform/Window.h"

#include <cmath>

namespace cfw {

Window::~Window() = default;

void Window::deliverTouch(PointerEvent event) {
    // The buttons a contact has are the mouse's left one, on both signals.
    const bool pressOrRelease = event.type == PointerEvent::Type::Press || event.type == PointerEvent::Type::Release ||
                                event.type == PointerEvent::Type::Cancel;
    event.button = pressOrRelease ? PointerButton::Left : PointerButton::None;
    event.clickCount = 1;
    if (event.primary && event.type == PointerEvent::Type::Press) {
        // A second tap within 400 ms and 12 logical pixels of the first is
        // a double click (a finger is less exact than a mouse).
        const TimePoint now = Clock::now();
        const Vec2 moved = event.position - m_lastTapAt;
        const bool again = m_tapCount > 0 && now - m_lastTapTime <= std::chrono::milliseconds(400) &&
                           std::abs(moved.x) <= 12.0f && std::abs(moved.y) <= 12.0f;
        m_tapCount = again ? m_tapCount + 1 : 1;
        m_lastTapTime = now;
        m_lastTapAt = event.position;
        event.clickCount = m_tapCount;
    }
    touch.emit(event);
    if (event.primary) {
        pointer.emit(event);
    }
}

Vec2 Window::logicalSize() const {
    const Vec2i px = pixelSize();
    const float ratio = devicePixelRatio();
    return {float(px.x) / ratio, float(px.y) / ratio};
}

} // namespace cfw
