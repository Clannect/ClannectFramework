#include "cfw/platform/Window.h"

namespace cfw {

Window::~Window() = default;

Vec2 Window::logicalSize() const {
    const Vec2i px = pixelSize();
    const float ratio = devicePixelRatio();
    return {float(px.x) / ratio, float(px.y) / ratio};
}

} // namespace cfw
