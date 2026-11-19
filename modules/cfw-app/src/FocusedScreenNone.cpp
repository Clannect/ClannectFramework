// focusedScreen where the system offers no way to ask (macOS for now).

#include "cfw/app/Desktop.h"

namespace cfw {

Result<FocusedScreen> focusedScreen() {
    return Error(ErrorCode::Unsupported, "the focused window's screen is not known on this system");
}

} // namespace cfw
