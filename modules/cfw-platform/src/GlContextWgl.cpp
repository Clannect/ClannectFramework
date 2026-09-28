// WGL: a legacy context bootstraps wglCreateContextAttribsARB, which then
// makes the core context. opengl32.dll is part of Windows.

#include "cfw/platform/Gl.h"
#include "cfw/platform/GlContext.h"
#include "cfw/platform/Window.h"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace cfw {

namespace {

constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_FLAGS_ARB = 0x2094;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;
constexpr int WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB = 0x00000002;
constexpr int WGL_CONTEXT_DEBUG_BIT_ARB = 0x0001;

using CreateContextAttribsFn = HGLRC(WINAPI *)(HDC, HGLRC, const int *);
using SwapIntervalFn = BOOL(WINAPI *)(int);

HMODULE opengl32() {
    static HMODULE module = LoadLibraryW(L"opengl32.dll");
    return module;
}

class GlContextWgl final : public GlContext {
public:
    GlContextWgl(HWND hwnd, HDC dc, HGLRC context) : m_hwnd(hwnd), m_dc(dc), m_context(context) {
        m_swapInterval = reinterpret_cast<SwapIntervalFn>(reinterpret_cast<void *>(wglGetProcAddress("wglSwapIntervalEXT")));
    }
    ~GlContextWgl() override {
        if (wglGetCurrentContext() == m_context) {
            wglMakeCurrent(nullptr, nullptr);
        }
        wglDeleteContext(m_context);
        ReleaseDC(m_hwnd, m_dc);
    }
    bool makeCurrent() override { return wglMakeCurrent(m_dc, m_context) != FALSE; }
    void doneCurrent() override { wglMakeCurrent(nullptr, nullptr); }
    void swapBuffers() override { SwapBuffers(m_dc); }
    void setSwapInterval(int interval) override {
        if (m_swapInterval) {
            m_swapInterval(interval);
        }
    }
    void *procAddress(const char *name) const override {
        void *p = reinterpret_cast<void *>(wglGetProcAddress(name));
        // Some drivers return small sentinels instead of null; GL 1.1
        // functions come from opengl32.dll itself.
        const auto value = reinterpret_cast<std::intptr_t>(p);
        if (value == 0 || value == 1 || value == 2 || value == 3 || value == -1) {
            p = reinterpret_cast<void *>(GetProcAddress(opengl32(), name));
        }
        return p;
    }
    void finish() { describe(); }
    [[nodiscard]] HGLRC handle() const noexcept { return m_context; }

private:
    HWND m_hwnd;
    HDC m_dc;
    HGLRC m_context;
    SwapIntervalFn m_swapInterval = nullptr;
};

} // namespace

Result<std::unique_ptr<GlContext>> GlContext::create(Window &window, const GlOptions &options) {
    HWND hwnd = static_cast<HWND>(window.nativeHandle());
    HDC dc = GetDC(hwnd); // the window class has CS_OWNDC
    if (!dc || !opengl32()) {
        return Error(ErrorCode::Unsupported, "OpenGL is not available");
    }
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    if (GetPixelFormat(dc) == 0) {
        const int format = ChoosePixelFormat(dc, &pfd);
        if (format == 0 || !SetPixelFormat(dc, format, &pfd)) {
            ReleaseDC(hwnd, dc);
            return Error(ErrorCode::Unsupported, "no OpenGL pixel format for the window");
        }
    }
    // A legacy context only to find wglCreateContextAttribsARB.
    HGLRC bootstrap = wglCreateContext(dc);
    if (!bootstrap || !wglMakeCurrent(dc, bootstrap)) {
        if (bootstrap) {
            wglDeleteContext(bootstrap);
        }
        ReleaseDC(hwnd, dc);
        return Error(ErrorCode::Unsupported, "could not create an OpenGL context");
    }
    const auto createContextAttribs = reinterpret_cast<CreateContextAttribsFn>(
        reinterpret_cast<void *>(wglGetProcAddress("wglCreateContextAttribsARB")));
    HGLRC context = nullptr;
    if (createContextAttribs) {
        const int flags = options.debug ? WGL_CONTEXT_DEBUG_BIT_ARB : 0;
        const int profile =
            options.coreProfile ? WGL_CONTEXT_CORE_PROFILE_BIT_ARB : WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
        const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, options.majorVersion, WGL_CONTEXT_MINOR_VERSION_ARB,
                               options.minorVersion, WGL_CONTEXT_FLAGS_ARB, flags, WGL_CONTEXT_PROFILE_MASK_ARB,
                               profile, 0};
        HGLRC share = options.shareWith ? static_cast<const GlContextWgl *>(options.shareWith)->handle() : nullptr;
        context = createContextAttribs(dc, share, attribs);
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(bootstrap);
    if (!context) {
        ReleaseDC(hwnd, dc);
        return Error(ErrorCode::Unsupported, "the driver cannot create an OpenGL " + std::to_string(options.majorVersion) +
                                                 "." + std::to_string(options.minorVersion) + " core context");
    }
    auto result = std::make_unique<GlContextWgl>(hwnd, dc, context);
    if (!result->makeCurrent()) {
        return Error(ErrorCode::Unsupported, "could not make the OpenGL context current");
    }
    result->finish();
    result->setSwapInterval(options.vsync ? 1 : 0);
    return std::unique_ptr<GlContext>(std::move(result));
}

} // namespace cfw
