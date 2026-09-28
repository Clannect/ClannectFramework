// No window system backend on this platform yet.

#include "cfw/platform/Window.h"

namespace cfw {

Result<std::unique_ptr<Window>> Window::create(const WindowOptions &) {
    return Error(ErrorCode::Unsupported, "no window system backend on this platform");
}
bool processEvents(Duration) { return false; }
void wakeUp() {}

namespace {
String &clipboard() {
    static String text;
    return text;
}
} // namespace

String clipboardText() { return clipboard(); }
void setClipboardText(StringView text) { clipboard() = String(text); }

} // namespace cfw
