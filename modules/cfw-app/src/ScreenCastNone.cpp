// ScreenCast where the system has no portal (Windows, macOS): unsupported;
// applications capture with the platform's own APIs.

#include "cfw/app/ScreenCast.h"

namespace cfw {

struct ScreenCast::Impl {};

ScreenCast::ScreenCast(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
ScreenCast::~ScreenCast() = default;
int ScreenCast::pipeWireFd() const noexcept { return -1; }
std::uint32_t ScreenCast::nodeId() const noexcept { return 0; }
Vec2i ScreenCast::size() const noexcept { return {}; }
const std::vector<ScreenCastStream> &ScreenCast::streams() const noexcept {
    static const std::vector<ScreenCastStream> none;
    return none;
}
const String &ScreenCast::restoreToken() const noexcept {
    static const String none;
    return none;
}
bool ScreenCast::available() { return false; }
Result<std::unique_ptr<ScreenCast>> ScreenCast::start(const ScreenCastOptions &) {
    return Error(ErrorCode::Unsupported, "screen casting through a portal is not available here");
}
Result<Path> takeScreenshot(std::chrono::seconds) {
    return Error(ErrorCode::Unsupported, "screenshots through a portal are not available here");
}

} // namespace cfw
