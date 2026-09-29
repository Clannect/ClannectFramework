// macOS: NSOpenGLContext on the window's content view. Apple's OpenGL stops
// at 4.1 core (deprecated, still shipped); asking for 3.2 core or newer
// gets the newest core context, so 3.3 requests are met by 4.1. Functions
// come from the OpenGL framework with dlsym.

#define GL_SILENCE_DEPRECATION 1

#import <AppKit/AppKit.h>

#include <dlfcn.h>

#include "cfw/platform/Gl.h"
#include "cfw/platform/GlContext.h"
#include "cfw/platform/Window.h"

namespace cfw {

namespace {

void *openGlFramework() {
    static void *library = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW | RTLD_LOCAL);
    return library;
}

class GlContextCocoa final : public GlContext {
public:
    GlContextCocoa(NSOpenGLContext *context, NSView *view) : m_context(context), m_view(view) {
        // The drawable follows the view's size: update before the next frame.
        [m_view setPostsFrameChangedNotifications:YES];
        m_observer = [[NSNotificationCenter defaultCenter] addObserverForName:NSViewFrameDidChangeNotification
                                                                       object:m_view
                                                                        queue:nil
                                                                   usingBlock:^(NSNotification *) {
                                                                     m_stale = true;
                                                                   }];
    }

    ~GlContextCocoa() override {
        [[NSNotificationCenter defaultCenter] removeObserver:m_observer];
        if ([NSOpenGLContext currentContext] == m_context) {
            [NSOpenGLContext clearCurrentContext];
        }
        [m_context clearDrawable];
    }

    bool makeCurrent() override {
        [m_context makeCurrentContext];
        if (m_stale) {
            m_stale = false;
            [m_context update];
        }
        return [NSOpenGLContext currentContext] == m_context;
    }

    void doneCurrent() override { [NSOpenGLContext clearCurrentContext]; }

    void swapBuffers() override { [m_context flushBuffer]; }

    void setSwapInterval(int interval) override {
        const GLint value = interval;
        [m_context setValues:&value forParameter:NSOpenGLContextParameterSwapInterval];
    }

    void *procAddress(const char *name) const override {
        void *library = openGlFramework();
        return library ? dlsym(library, name) : nullptr;
    }

    void finish() { describe(); }
    [[nodiscard]] NSOpenGLContext *handle() const { return m_context; }

private:
    NSOpenGLContext *m_context;
    NSView *m_view;
    id m_observer = nil;
    bool m_stale = false;
};

} // namespace

Result<std::unique_ptr<GlContext>> GlContext::create(Window &window, const GlOptions &options) {
    @autoreleasepool {
        NSView *view = (__bridge NSView *)window.nativeHandle();
        if (!view || !openGlFramework()) {
            return Error(ErrorCode::Unsupported, "OpenGL is not available");
        }
        const bool core = options.coreProfile && options.majorVersion >= 3;
        if (core && (options.majorVersion > 4 || (options.majorVersion == 4 && options.minorVersion > 1))) {
            return Error(ErrorCode::Unsupported, "macOS offers OpenGL up to 4.1");
        }
        if (!core && options.majorVersion > 2) {
            return Error(ErrorCode::Unsupported, "macOS offers OpenGL 3 and newer only as a core profile");
        }
        const NSOpenGLPixelFormatAttribute attributes[] = {
            NSOpenGLPFAOpenGLProfile,
            NSOpenGLPixelFormatAttribute(core ? NSOpenGLProfileVersion4_1Core : NSOpenGLProfileVersionLegacy),
            NSOpenGLPFADoubleBuffer,
            NSOpenGLPFAAccelerated,
            NSOpenGLPFAColorSize, 24,
            NSOpenGLPFAAlphaSize, 8,
            NSOpenGLPFADepthSize, 24,
            NSOpenGLPFAStencilSize, 8,
            0,
        };
        NSOpenGLPixelFormat *format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];
        if (!format) {
            return Error(ErrorCode::Unsupported, "no OpenGL pixel format with the requested buffers");
        }
        NSOpenGLContext *share = nil;
        if (options.shareWith) {
            share = static_cast<const GlContextCocoa *>(options.shareWith)->handle();
        }
        NSOpenGLContext *context = [[NSOpenGLContext alloc] initWithFormat:format shareContext:share];
        if (!context) {
            return Error(ErrorCode::Unsupported, "could not create an OpenGL context");
        }
        [view setWantsBestResolutionOpenGLSurface:YES]; // physical pixels on Retina displays
        [context setView:view];
        auto result = std::make_unique<GlContextCocoa>(context, view);
        if (!result->makeCurrent()) {
            return Error(ErrorCode::Unsupported, "could not make the OpenGL context current");
        }
        result->finish();
        if (result->majorVersion() < options.majorVersion ||
            (result->majorVersion() == options.majorVersion && result->minorVersion() < options.minorVersion)) {
            return Error(ErrorCode::Unsupported, "OpenGL " + std::to_string(options.majorVersion) + "." +
                                                     std::to_string(options.minorVersion) + " is not available");
        }
        result->setSwapInterval(options.vsync ? 1 : 0);
        return std::unique_ptr<GlContext>(std::move(result));
    }
}

} // namespace cfw
