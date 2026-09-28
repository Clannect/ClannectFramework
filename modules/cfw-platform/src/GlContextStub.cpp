#include "cfw/platform/GlContext.h"

namespace cfw {

Result<std::unique_ptr<GlContext>> GlContext::create(Window &, const GlOptions &) {
    return Error(ErrorCode::Unsupported, "no OpenGL on this platform");
}

} // namespace cfw
