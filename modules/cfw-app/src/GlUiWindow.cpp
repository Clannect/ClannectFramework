#include "cfw/app/GlUiWindow.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"

namespace cfw {

namespace {

// The interface layer: a premultiplied image stretched over the window,
// row 0 at the top.
constexpr const char *kVertexShader = R"(#version 330 core
out vec2 vUv;
void main() {
    vec2 corners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
    vec2 corner = corners[gl_VertexID];
    vUv = vec2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    gl_Position = vec4(corner, 0.0, 1.0);
}
)";

constexpr const char *kFragmentShader = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uLayer;
out vec4 fragColor;
void main() {
    fragColor = texture(uLayer, vUv);
}
)";

GLuint compile(const GlFunctions &gl, GLenum stage, const char *source, String *error) {
    const GLuint shader = gl.CreateShader(stage);
    gl.ShaderSource(shader, 1, &source, nullptr);
    gl.CompileShader(shader);
    GLint ok = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        gl.GetShaderInfoLog(shader, sizeof log, nullptr, log);
        if (error) {
            *error = String("interface shader: ") + log;
        }
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

// A view's rectangle in window pixels.
struct PixelRect {
    int x, y, width, height;
};

PixelRect toPixels(const RectF &r, float ratio) {
    const int x0 = int(std::lround(r.x * ratio));
    const int y0 = int(std::lround(r.y * ratio));
    const int x1 = int(std::lround(r.right() * ratio));
    const int y1 = int(std::lround(r.bottom() * ratio));
    return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

} // namespace

// ---- GlView ------------------------------------------------------------------------------

GlView::GlView() {
    setRole(Role::Group);
    setAccessibleName("3D view");
    setFocusable(true);
}

GlView::~GlView() {
    if (m_gl) {
        if (m_target.framebuffer) {
            m_gl->DeleteFramebuffers(1, &m_target.framebuffer);
        }
        if (m_target.color) {
            m_gl->DeleteTextures(1, &m_target.color);
        }
        if (m_target.depth) {
            m_gl->DeleteRenderbuffers(1, &m_target.depth);
        }
    }
}

void GlView::paint(Painter &painter, const Theme &) {
    // A hole in the interface layer: the view shows through it.
    painter.save();
    painter.setBlendMode(BlendMode::Source);
    painter.fillRect(rect(), Color{0, 0, 0, 0});
    painter.restore();
}

// ---- GlUiWindow --------------------------------------------------------------------------

Result<std::unique_ptr<GlUiWindow>> GlUiWindow::create(const WindowOptions &options, Theme theme,
                                                       const GlOptions &glOptions) {
    auto window = Window::create(options);
    if (!window) {
        return window.error();
    }
    auto context = GlContext::create(*window.value(), glOptions);
    if (!context) {
        return context.error();
    }
    std::unique_ptr<GlUiWindow> ui(
        new GlUiWindow(std::move(window).value(), std::move(context).value(), std::move(theme)));
    String error;
    if (!ui->initialize(&error)) {
        return Error(ErrorCode::Unsupported, error);
    }
    return ui;
}

GlUiWindow::GlUiWindow(std::unique_ptr<Window> window, std::unique_ptr<GlContext> context, Theme theme)
    : m_window(std::move(window)), m_context(std::move(context)), m_surface(std::move(theme)) {
    m_surface.readClipboard = [] { return clipboardText(); };
    m_surface.movePointer = [this](Vec2 position) { m_window->setPointerPosition(position); };
    m_surface.writeClipboard = [](StringView text) { setClipboardText(text); };
    m_connections.push_back(m_window->pointer.connect([this](const PointerEvent &e) {
        m_surface.dispatch(e);
        updateCursor();
    }));
    m_connections.push_back(m_window->key.connect([this](const KeyEvent &e) { m_surface.dispatch(e); }));
    m_connections.push_back(m_window->text.connect([this](const TextEvent &e) { m_surface.dispatch(e); }));
    m_window->setDropHandler([this](const DropEvent &e) { return m_surface.dispatch(e); });
    m_connections.push_back(m_window->resized.connect([this](Vec2i) { resize(); }));
    m_connections.push_back(m_window->dpiChanged.connect([this](float) { resize(); }));
    m_connections.push_back(m_window->repaintRequested.connect([this] { m_fullRedraw = true; }));
    m_connections.push_back(m_window->closeRequested.connect([this] {
        if (m_closeHandler) {
            m_closeHandler();
        } else {
            m_open = false;
        }
    }));
    m_connections.push_back(m_window->focusChanged.connect([this](bool focused) {
        if (!focused) {
            m_surface.dispatch(PointerEvent{PointerEvent::Type::Leave});
        }
    }));
}

bool GlUiWindow::initialize(String *error) {
    m_context->makeCurrent();
    const char *missing = nullptr;
    if (!m_gl.load(*m_context, &missing)) {
        if (error) {
            *error = String("OpenGL function missing: ") + (missing ? missing : "?");
        }
        return false;
    }
    const GLuint vertex = compile(m_gl, GL_VERTEX_SHADER, kVertexShader, error);
    const GLuint fragment = vertex ? compile(m_gl, GL_FRAGMENT_SHADER, kFragmentShader, error) : 0;
    if (!vertex || !fragment) {
        return false;
    }
    m_program = m_gl.CreateProgram();
    m_gl.AttachShader(m_program, vertex);
    m_gl.AttachShader(m_program, fragment);
    m_gl.LinkProgram(m_program);
    m_gl.DeleteShader(vertex);
    m_gl.DeleteShader(fragment);
    GLint linked = 0;
    m_gl.GetProgramiv(m_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        if (error) {
            *error = "interface shader: link failed";
        }
        return false;
    }
    m_gl.GenVertexArrays(1, &m_vao);
    m_gl.GenTextures(1, &m_layerTexture);
    m_gl.BindTexture(GL_TEXTURE_2D, m_layerTexture);
    m_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GLint(GL_NEAREST));
    m_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GLint(GL_NEAREST));
    m_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLint(GL_CLAMP_TO_EDGE));
    m_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLint(GL_CLAMP_TO_EDGE));
    m_gl.BindTexture(GL_TEXTURE_2D, 0);
    resize();
    return true;
}

GlUiWindow::~GlUiWindow() {
    m_connections.clear();
    m_context->makeCurrent();
    // The views free their framebuffers as the surface's tree goes.
    m_surface.setRoot(nullptr);
    m_surface.closePopups();
    if (m_gl.DeleteTextures && m_layerTexture) {
        m_gl.DeleteTextures(1, &m_layerTexture);
    }
    if (m_gl.DeleteVertexArrays && m_vao) {
        m_gl.DeleteVertexArrays(1, &m_vao);
    }
    if (m_gl.DeleteProgram && m_program) {
        m_gl.DeleteProgram(m_program);
    }
}

void GlUiWindow::resize() {
    m_surface.setSize(m_window->logicalSize());
    const Vec2i pixels = m_window->pixelSize();
    auto layer = Image::create(std::uint32_t(std::max(1, pixels.x)), std::uint32_t(std::max(1, pixels.y)),
                               AlphaMode::Premultiplied);
    if (layer) {
        m_layer = std::move(layer).value();
    }
    m_layerDirty = true;
    m_fullRedraw = true;
}

void GlUiWindow::updateCursor() {
    const Cursor wanted = m_surface.cursor();
    if (wanted != m_cursor) {
        m_cursor = wanted;
        m_window->setCursor(wanted);
    }
}

void GlUiWindow::collectViews(Element &element, std::vector<GlView *> &out) {
    if (!element.isVisible() || element.rect().isEmpty()) {
        return;
    }
    if (auto *view = dynamic_cast<GlView *>(&element)) {
        out.push_back(view);
    }
    for (const auto &child : element.children()) {
        collectViews(*child, out);
    }
}

void GlUiWindow::renderView(GlView &view, int width, int height) {
    GlView::Target &t = view.m_target;
    if (!t.framebuffer) {
        view.m_gl = &m_gl;
        m_gl.GenFramebuffers(1, &t.framebuffer);
        m_gl.GenTextures(1, &t.color);
        m_gl.GenRenderbuffers(1, &t.depth);
    }
    if (t.width != width || t.height != height) {
        m_gl.BindTexture(GL_TEXTURE_2D, t.color);
        m_gl.TexImage2D(GL_TEXTURE_2D, 0, GLint(GL_RGBA8), width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        m_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GLint(GL_NEAREST));
        m_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GLint(GL_NEAREST));
        m_gl.BindTexture(GL_TEXTURE_2D, 0);
        m_gl.BindRenderbuffer(GL_RENDERBUFFER, t.depth);
        m_gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
        m_gl.BindRenderbuffer(GL_RENDERBUFFER, 0);
        m_gl.BindFramebuffer(GL_FRAMEBUFFER, t.framebuffer);
        m_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
        m_gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.depth);
        t.width = width;
        t.height = height;
    }
    m_gl.BindFramebuffer(GL_FRAMEBUFFER, t.framebuffer);
    m_gl.Viewport(0, 0, width, height);
    m_gl.ClearColor(view.background.r, view.background.g, view.background.b, 1.0f);
    m_gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (view.render) {
        GlViewTarget target;
        target.gl = &m_gl;
        target.framebuffer = t.framebuffer;
        target.width = width;
        target.height = height;
        target.devicePixelRatio = m_window->devicePixelRatio();
        view.render(target);
    }
    view.m_dirty = false;
    m_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool GlUiWindow::wantsFrame() {
    m_surface.layout();
    if (m_fullRedraw || m_layerDirty || m_surface.needsLayout()) {
        return true;
    }
    std::vector<GlView *> views;
    collectViews(m_surface.root(), views);
    return std::any_of(views.begin(), views.end(), [](const GlView *v) { return v->needsRender(); });
}

void GlUiWindow::composite(const std::vector<GlView *> &views, Vec2i pixels) {
    const float ratio = m_window->devicePixelRatio();
    m_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    m_gl.Viewport(0, 0, pixels.x, pixels.y);
    m_gl.Disable(GL_SCISSOR_TEST);
    const Color back = m_surface.theme().window;
    m_gl.ClearColor(back.r, back.g, back.b, 1.0f);
    m_gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Views, where they are laid out (GL counts rows from the bottom).
    for (GlView *view : views) {
        const PixelRect p = toPixels(view->rect(), ratio);
        const GlView::Target &t = view->m_target;
        if (!t.framebuffer || p.width == 0 || p.height == 0) {
            continue;
        }
        m_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, t.framebuffer);
        m_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        m_gl.BlitFramebuffer(0, 0, t.width, t.height, p.x, pixels.y - p.y - p.height, p.x + p.width,
                             pixels.y - p.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    m_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

    // The interface over them.
    m_gl.Disable(GL_DEPTH_TEST);
    m_gl.Disable(GL_CULL_FACE);
    m_gl.Enable(GL_BLEND);
    m_gl.BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    m_gl.UseProgram(m_program);
    m_gl.ActiveTexture(GL_TEXTURE0);
    m_gl.BindTexture(GL_TEXTURE_2D, m_layerTexture);
    m_gl.Uniform1i(m_gl.GetUniformLocation(m_program, "uLayer"), 0);
    m_gl.BindVertexArray(m_vao);
    m_gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_gl.BindVertexArray(0);
    m_gl.BindTexture(GL_TEXTURE_2D, 0);
    m_gl.UseProgram(0);
    m_gl.Disable(GL_BLEND);
}

bool GlUiWindow::frame() {
    m_surface.runTimers();
    updateCursor();
    m_surface.layout();
    if (!m_surface.takeDamage().isEmpty()) {
        m_layerDirty = true;
    }
    std::vector<GlView *> views;
    collectViews(m_surface.root(), views);
    const float ratio = m_window->devicePixelRatio();
    bool viewsChanged = false;
    for (GlView *view : views) {
        const PixelRect p = toPixels(view->rect(), ratio);
        viewsChanged = viewsChanged || view->needsRender() || p.width != view->m_target.width ||
                       p.height != view->m_target.height;
    }
    if (!m_fullRedraw && !m_layerDirty && !viewsChanged) {
        return false;
    }
    m_fullRedraw = false;
    m_context->makeCurrent();
    const Vec2i pixels{std::max(1, m_window->pixelSize().x), std::max(1, m_window->pixelSize().y)};

    for (GlView *view : views) {
        const PixelRect p = toPixels(view->rect(), ratio);
        if (p.width > 0 && p.height > 0 &&
            (view->needsRender() || p.width != view->m_target.width || p.height != view->m_target.height)) {
            renderView(*view, p.width, p.height);
        }
    }

    if (m_layerDirty) {
        m_layerDirty = false;
        std::fill(m_layer.pixels().begin(), m_layer.pixels().end(), std::uint8_t(0));
        {
            RasterPaintBackend backend(m_layer);
            Painter painter(backend);
            painter.scale(ratio, ratio);
            m_surface.paint(painter);
        }
        m_gl.BindTexture(GL_TEXTURE_2D, m_layerTexture);
        m_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        m_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, GLint(m_layer.stride() / 4));
        m_gl.TexImage2D(GL_TEXTURE_2D, 0, GLint(GL_RGBA8), GLsizei(m_layer.width()), GLsizei(m_layer.height()), 0,
                        GL_RGBA, GL_UNSIGNED_BYTE, m_layer.pixels().data());
        m_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        m_gl.BindTexture(GL_TEXTURE_2D, 0);
    }

    composite(views, pixels);
    m_context->swapBuffers();
    ++m_frames;
    return true;
}

Result<Image> GlUiWindow::captureFrame() {
    m_context->makeCurrent();
    std::vector<GlView *> views;
    collectViews(m_surface.root(), views);
    const Vec2i pixels{std::max(1, m_window->pixelSize().x), std::max(1, m_window->pixelSize().y)};
    // Composite again into the back buffer and read it before any swap.
    composite(views, pixels);
    auto created = Image::create(std::uint32_t(pixels.x), std::uint32_t(pixels.y), AlphaMode::Straight);
    if (!created) {
        return created.error();
    }
    Image image = std::move(created).value();
    std::vector<std::uint8_t> rows(std::size_t(pixels.x) * std::size_t(pixels.y) * 4);
    m_gl.ReadBuffer(GL_BACK);
    m_gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    m_gl.ReadPixels(0, 0, pixels.x, pixels.y, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
    const std::size_t rowBytes = std::size_t(pixels.x) * 4;
    for (int y = 0; y < pixels.y; ++y) {
        std::uint8_t *out = image.pixels().data() + std::size_t(y) * image.stride();
        std::memcpy(out, rows.data() + std::size_t(pixels.y - 1 - y) * rowBytes, rowBytes);
        for (std::size_t x = 3; x < rowBytes; x += 4) {
            out[x] = 255; // the window is opaque
        }
    }
    return image;
}

void runUntilClosed(GlUiWindow &window) {
    while (window.isOpen()) {
        window.frame();
        Duration wait = std::chrono::milliseconds(250);
        if (window.wantsFrame()) {
            wait = Duration::zero(); // an animating view: the swap interval paces us
        } else if (const std::optional<TimePoint> next = window.surface().nextTimer()) {
            wait = std::clamp(std::chrono::duration_cast<Duration>(*next - Clock::now()), Duration::zero(), wait);
        }
        processEvents(wait);
    }
}

} // namespace cfw
