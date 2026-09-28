#pragma once

// A native window with an OpenGL context whose contents are a cfw-ui Surface
// with 3D views in it (the editor: panels around a viewport).
//
// Each GlView renders into a framebuffer of its own, sized to its rectangle
// in pixels, so a renderer draws as if it had the whole window. A frame:
//   1. the views that need it render (every frame while animating, else
//      after update() or a resize);
//   2. the views are copied into the window where they are laid out;
//   3. the interface, painted on the CPU into a premultiplied layer (only
//      when it changed) with holes where the views are, is drawn on top;
//   4. the buffers swap.
// Popups and tooltips lie over views like anything else, because the
// interface layer is drawn last.
//
// Idle when nothing changes, like UiWindow: runUntilClosed sleeps until
// input, a timer, or a view that animates or asked for an update.
//
// Threads: the window's thread; the context is current on it.

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/image/Image.h"
#include "cfw/platform/Gl.h"
#include "cfw/platform/GlContext.h"
#include "cfw/platform/Window.h"
#include "cfw/ui/Surface.h"

namespace cfw {

class GlUiWindow;

// What a view renders into this frame.
struct GlViewTarget {
    const GlFunctions *gl = nullptr;
    GLuint framebuffer = 0; // bound when render runs; colour and depth
    int width = 1;          // pixels
    int height = 1;
    float devicePixelRatio = 1.0f;
};

// An element drawn with OpenGL. It takes input like any element (a
// viewport's camera drags, clicks, keys when focused).
class GlView : public Element {
public:
    GlView();
    // Frees its framebuffer; the window's context must be current (it is,
    // on the window's thread).
    ~GlView() override;

    // Draws the view. Leave the framebuffer bound (and the context current).
    std::function<void(const GlViewTarget &)> render;
    // Redraws every frame (play mode, a moving camera); otherwise only after
    // update(), a resize or the first show.
    void setAnimating(bool animating) noexcept { m_animating = animating; }
    [[nodiscard]] bool isAnimating() const noexcept { return m_animating; }
    void update() noexcept { m_dirty = true; }
    [[nodiscard]] bool needsRender() const noexcept { return m_dirty || m_animating; }
    // The colour the view shows before its first render.
    Color background{0, 0, 0, 1};

    // Leaves its rectangle transparent in the interface layer.
    void paint(Painter &painter, const Theme &theme) override;

private:
    friend class GlUiWindow;
    struct Target {
        GLuint framebuffer = 0;
        GLuint color = 0;
        GLuint depth = 0;
        int width = 0;
        int height = 0;
    };
    Target m_target;
    const GlFunctions *m_gl = nullptr; // set while it has GL objects
    bool m_animating = false;
    bool m_dirty = true;
};

class GlUiWindow {
public:
    [[nodiscard]] static Result<std::unique_ptr<GlUiWindow>> create(const WindowOptions &options, Theme theme,
                                                                    const GlOptions &gl = {});
    ~GlUiWindow();
    GlUiWindow(const GlUiWindow &) = delete;
    GlUiWindow &operator=(const GlUiWindow &) = delete;

    [[nodiscard]] Surface &surface() noexcept { return m_surface; }
    [[nodiscard]] Window &window() noexcept { return *m_window; }
    [[nodiscard]] GlContext &context() noexcept { return *m_context; }
    [[nodiscard]] const GlFunctions &gl() const noexcept { return m_gl; }
    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    void close() noexcept { m_open = false; }
    // What the window's close button does. Unset, it closes the window;
    // set, the handler decides (after asking about unsaved work, say) and
    // calls close() itself when the window should go.
    void setCloseHandler(std::function<void()> handler) { m_closeHandler = std::move(handler); }

    // Renders and presents if anything changed. Returns whether it did.
    bool frame();
    // Whether the next frame() has anything to do without new input.
    [[nodiscard]] bool wantsFrame();
    [[nodiscard]] std::size_t framesPresented() const noexcept { return m_frames; }
    // The last presented frame, read back (for tests and screenshots).
    [[nodiscard]] Result<Image> captureFrame();

private:
    GlUiWindow(std::unique_ptr<Window> window, std::unique_ptr<GlContext> context, Theme theme);
    bool initialize(String *error);
    void resize();
    void updateCursor();
    void collectViews(Element &element, std::vector<GlView *> &out);
    void renderView(GlView &view, int width, int height);
    void composite(const std::vector<GlView *> &views, Vec2i pixels);

    std::unique_ptr<Window> m_window;
    std::unique_ptr<GlContext> m_context;
    GlFunctions m_gl;
    Surface m_surface;
    Image m_layer;
    bool m_layerDirty = true;
    bool m_fullRedraw = true;
    GLuint m_layerTexture = 0;
    GLuint m_program = 0;
    GLuint m_vao = 0;
    std::vector<ScopedConnection> m_connections;
    bool m_open = true;
    std::optional<RectF> m_textInputArea; // last told to the window
    bool m_textInputKnown = false;
    std::function<void()> m_closeHandler;
    std::size_t m_frames = 0;
    Cursor m_cursor = Cursor::Arrow;
};

// Runs until the window is closed: events, timers, frames.
void runUntilClosed(GlUiWindow &window);

} // namespace cfw
