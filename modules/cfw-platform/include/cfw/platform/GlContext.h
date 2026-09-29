#pragma once

// An OpenGL context drawing into a Window (WGL on Windows, GLX on X11,
// NSOpenGLContext on macOS, where 3.3 core requests get 4.1 core).
// Core profile, version 3.3 or newer by default, double-buffered with a
// 24-bit depth and 8-bit stencil buffer.
//
// A window that has a GL context is drawn by GL: do not also call
// Window::present() on it.
//
// Threads: a context is current on at most one thread at a time.

#include <memory>

#include "cfw/core/Result.h"
#include "cfw/core/String.h"

namespace cfw {

class Window;

struct GlOptions {
    int majorVersion = 3;
    int minorVersion = 3;
    bool coreProfile = true;
    bool debug = false;
    bool vsync = true;
    // Objects (textures, buffers, programs) are shared with this context.
    const class GlContext *shareWith = nullptr;
};

class GlContext {
public:
    // Fails with Unsupported when there is no OpenGL, or the version is not
    // available.
    [[nodiscard]] static Result<std::unique_ptr<GlContext>> create(Window &window, const GlOptions &options = {});
    virtual ~GlContext();
    GlContext(const GlContext &) = delete;
    GlContext &operator=(const GlContext &) = delete;

    virtual bool makeCurrent() = 0;
    virtual void doneCurrent() = 0;
    virtual void swapBuffers() = 0;
    // 1: wait for vertical blank, 0: do not (where the driver allows).
    virtual void setSwapInterval(int interval) = 0;
    // A GL function by its full name ("glClear"); null if absent.
    [[nodiscard]] virtual void *procAddress(const char *name) const = 0;

    [[nodiscard]] int majorVersion() const noexcept { return m_major; }
    [[nodiscard]] int minorVersion() const noexcept { return m_minor; }
    [[nodiscard]] const String &renderer() const noexcept { return m_renderer; }

protected:
    GlContext() = default;
    // Reads the version and renderer (the context must be current).
    void describe();
    int m_major = 0;
    int m_minor = 0;
    String m_renderer;
};

} // namespace cfw
