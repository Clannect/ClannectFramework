#pragma once

// Build configuration visible to C++. The build system defines CFW_DEBUG_CHECKS
// for Debug builds; this is the only place that reads it.

namespace cfw {

#if defined(CFW_DEBUG_CHECKS)
inline constexpr bool kDebugChecks = true;
#else
inline constexpr bool kDebugChecks = false;
#endif

} // namespace cfw
