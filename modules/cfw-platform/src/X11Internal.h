#pragma once

// Shared between the X11 window backend and GLX (internal). Declared without
// Xlib types so including it does not bring Xlib's macros.

namespace cfw::detail {

// The process's X connection (a Display *), or null before the first window.
void *x11Display();

} // namespace cfw::detail
