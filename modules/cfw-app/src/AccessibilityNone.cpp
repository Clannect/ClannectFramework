#include "AccessibilityBridge.h"

namespace cfw::detail {

std::unique_ptr<AccessibilityBridge> createAccessibilityBridge(Window &, Surface &) { return nullptr; }

} // namespace cfw::detail
