// GLX: libGL is loaded at run time (dlopen), so the X11 backend still starts
// on a machine without OpenGL; GlContext::create() then reports Unsupported.

#include <dlfcn.h>

#include <vector>

#include "X11Internal.h"
#include "cfw/platform/Gl.h"
#include "cfw/platform/GlContext.h"
#include "cfw/platform/Window.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace cfw {

namespace {

// The GLX types and constants used here (glx.h / glxext.h values).
using GLXContext = struct __GLXcontextRec *;
using GLXFBConfig = struct __GLXFBConfigRec *;
using GLXDrawable = XID;
constexpr int GLX_X_RENDERABLE = 0x8012;
constexpr int GLX_DRAWABLE_TYPE = 0x8010;
constexpr int GLX_WINDOW_BIT = 0x00000001;
constexpr int GLX_RENDER_TYPE = 0x8011;
constexpr int GLX_RGBA_BIT = 0x00000001;
constexpr int GLX_DOUBLEBUFFER = 5;
constexpr int GLX_RED_SIZE = 8;
constexpr int GLX_GREEN_SIZE = 9;
constexpr int GLX_BLUE_SIZE = 10;
constexpr int GLX_DEPTH_SIZE = 12;
constexpr int GLX_STENCIL_SIZE = 13;
constexpr int GLX_VISUAL_ID = 0x800B;
constexpr int GLX_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int GLX_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int GLX_CONTEXT_FLAGS_ARB = 0x2094;
constexpr int GLX_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int GLX_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;
constexpr int GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB = 0x00000002;
constexpr int GLX_CONTEXT_DEBUG_BIT_ARB = 0x0001;

struct Glx {
    void *library = nullptr;
    void *(*getProcAddress)(const GLubyte *) = nullptr;
    GLXFBConfig *(*chooseFBConfig)(Display *, int, const int *, int *) = nullptr;
    int (*getFBConfigAttrib)(Display *, GLXFBConfig, int, int *) = nullptr;
    int (*makeContextCurrent)(Display *, GLXDrawable, GLXDrawable, GLXContext) = nullptr;
    void (*swapBuffers)(Display *, GLXDrawable) = nullptr;
    void (*destroyContext)(Display *, GLXContext) = nullptr;
    GLXContext (*createContextAttribs)(Display *, GLXFBConfig, GLXContext, int, const int *) = nullptr;
    void (*swapIntervalExt)(Display *, GLXDrawable, int) = nullptr;
    int (*swapIntervalMesa)(unsigned) = nullptr;

    bool load() {
        if (library) {
            return getProcAddress != nullptr;
        }
        library = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            library = dlopen("libGL.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (!library) {
            return false;
        }
        getProcAddress = reinterpret_cast<decltype(getProcAddress)>(dlsym(library, "glXGetProcAddressARB"));
        chooseFBConfig = reinterpret_cast<decltype(chooseFBConfig)>(dlsym(library, "glXChooseFBConfig"));
        getFBConfigAttrib = reinterpret_cast<decltype(getFBConfigAttrib)>(dlsym(library, "glXGetFBConfigAttrib"));
        makeContextCurrent = reinterpret_cast<decltype(makeContextCurrent)>(dlsym(library, "glXMakeContextCurrent"));
        swapBuffers = reinterpret_cast<decltype(swapBuffers)>(dlsym(library, "glXSwapBuffers"));
        destroyContext = reinterpret_cast<decltype(destroyContext)>(dlsym(library, "glXDestroyContext"));
        if (!getProcAddress || !chooseFBConfig || !getFBConfigAttrib || !makeContextCurrent || !swapBuffers ||
            !destroyContext) {
            getProcAddress = nullptr;
            return false;
        }
        createContextAttribs = reinterpret_cast<decltype(createContextAttribs)>(
            getProcAddress(reinterpret_cast<const GLubyte *>("glXCreateContextAttribsARB")));
        swapIntervalExt = reinterpret_cast<decltype(swapIntervalExt)>(
            getProcAddress(reinterpret_cast<const GLubyte *>("glXSwapIntervalEXT")));
        swapIntervalMesa = reinterpret_cast<decltype(swapIntervalMesa)>(
            getProcAddress(reinterpret_cast<const GLubyte *>("glXSwapIntervalMESA")));
        return true;
    }
};

Glx &glx() {
    static Glx g;
    return g;
}

class GlContextGlx final : public GlContext {
public:
    GlContextGlx(Display *display, ::Window window, GLXContext context)
        : m_display(display), m_window(window), m_context(context) {}
    ~GlContextGlx() override {
        glx().makeContextCurrent(m_display, 0, 0, nullptr);
        glx().destroyContext(m_display, m_context);
    }
    bool makeCurrent() override { return glx().makeContextCurrent(m_display, m_window, m_window, m_context) != 0; }
    void doneCurrent() override { glx().makeContextCurrent(m_display, 0, 0, nullptr); }
    void swapBuffers() override { glx().swapBuffers(m_display, m_window); }
    void setSwapInterval(int interval) override {
        if (glx().swapIntervalExt) {
            glx().swapIntervalExt(m_display, m_window, interval);
        } else if (glx().swapIntervalMesa) {
            glx().swapIntervalMesa(unsigned(interval));
        }
    }
    void *procAddress(const char *name) const override {
        return glx().getProcAddress(reinterpret_cast<const GLubyte *>(name));
    }
    void finish() { describe(); }

    [[nodiscard]] GLXContext handle() const noexcept { return m_context; }

private:
    Display *m_display;
    ::Window m_window;
    GLXContext m_context;
};

} // namespace

Result<std::unique_ptr<GlContext>> GlContext::create(Window &window, const GlOptions &options) {
    auto *display = static_cast<Display *>(detail::x11Display());
    Glx &g = glx();
    if (!display || !g.load() || !g.createContextAttribs) {
        return Error(ErrorCode::Unsupported, "OpenGL (GLX 1.4 with GLX_ARB_create_context) is not available");
    }
    const auto xwindow = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(window.nativeHandle()));
    XWindowAttributes attributes{};
    XGetWindowAttributes(display, xwindow, &attributes);
    const VisualID visual = XVisualIDFromVisual(attributes.visual);

    const int wanted[] = {GLX_X_RENDERABLE, 1, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
                          GLX_DOUBLEBUFFER, 1, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
                          GLX_DEPTH_SIZE, 24, GLX_STENCIL_SIZE, 8, 0};
    int count = 0;
    GLXFBConfig *configs = g.chooseFBConfig(display, DefaultScreen(display), wanted, &count);
    GLXFBConfig chosen = nullptr;
    // The config must match the window's visual (the window already exists).
    for (int i = 0; i < count && !chosen; ++i) {
        int id = 0;
        g.getFBConfigAttrib(display, configs[i], GLX_VISUAL_ID, &id);
        if (VisualID(id) == visual) {
            chosen = configs[i];
        }
    }
    if (configs) {
        XFree(configs);
    }
    if (!chosen) {
        return Error(ErrorCode::Unsupported, "no GLX framebuffer config matches the window's visual");
    }

    const int flags = options.debug ? GLX_CONTEXT_DEBUG_BIT_ARB : 0;
    const int profile =
        options.coreProfile ? GLX_CONTEXT_CORE_PROFILE_BIT_ARB : GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
    const int attribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, options.majorVersion, GLX_CONTEXT_MINOR_VERSION_ARB,
                           options.minorVersion, GLX_CONTEXT_FLAGS_ARB, flags, GLX_CONTEXT_PROFILE_MASK_ARB, profile, 0};
    GLXContext share = options.shareWith ? static_cast<const GlContextGlx *>(options.shareWith)->handle() : nullptr;
    // A failed creation raises an X error; catch it rather than exiting.
    static bool failed = false;
    failed = false;
    auto previous = XSetErrorHandler([](Display *, XErrorEvent *) {
        failed = true;
        return 0;
    });
    GLXContext context = g.createContextAttribs(display, chosen, share, 1, attribs);
    XSync(display, False);
    XSetErrorHandler(previous);
    if (!context || failed) {
        return Error(ErrorCode::Unsupported, "the driver cannot create an OpenGL " +
                                                 std::to_string(options.majorVersion) + "." +
                                                 std::to_string(options.minorVersion) + " context");
    }
    auto result = std::make_unique<GlContextGlx>(display, xwindow, context);
    if (!result->makeCurrent()) {
        return Error(ErrorCode::Unsupported, "could not make the OpenGL context current");
    }
    result->finish();
    result->setSwapInterval(options.vsync ? 1 : 0);
    return std::unique_ptr<GlContext>(std::move(result));
}

} // namespace cfw
