#pragma once

// Connects a window's surface to the platform's assistive technology:
// MSAA on Windows (WM_GETOBJECT, IAccessible, focus WinEvents), AT-SPI on
// Linux. UiWindow and GlUiWindow own one each.

#include <memory>

namespace cfw {
class Surface;
class Window;
} // namespace cfw

namespace cfw::detail {

class AccessibilityBridge {
public:
    virtual ~AccessibilityBridge() = default;
    // Called every frame: whatever the platform needs polled.
    virtual void update() {}
};

// Null where the platform has no bridge.
std::unique_ptr<AccessibilityBridge> createAccessibilityBridge(Window &window, Surface &surface);

} // namespace cfw::detail
