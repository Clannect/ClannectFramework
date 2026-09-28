#include "cfw/app/UiWindow.h"

#include <algorithm>
#include <cstring>

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/ui/Controls.h"

namespace cfw {

Result<std::unique_ptr<UiWindow>> UiWindow::create(const WindowOptions &options, Theme theme) {
    auto window = Window::create(options);
    if (!window) {
        return window.error();
    }
    return std::unique_ptr<UiWindow>(new UiWindow(std::move(window).value(), std::move(theme)));
}

UiWindow::UiWindow(std::unique_ptr<Window> window, Theme theme) : m_window(std::move(window)), m_surface(std::move(theme)) {
    m_surface.readClipboard = [] { return clipboardText(); };
    m_surface.writeClipboard = [](StringView text) { setClipboardText(text); };
    resize(m_window->pixelSize());
    m_connections.push_back(m_window->pointer.connect([this](const PointerEvent &e) {
        m_surface.dispatch(e);
        updateCursor();
    }));
    m_connections.push_back(m_window->key.connect([this](const KeyEvent &e) { m_surface.dispatch(e); }));
    m_connections.push_back(m_window->text.connect([this](const TextEvent &e) { m_surface.dispatch(e); }));
    m_connections.push_back(m_window->resized.connect([this](Vec2i pixels) { resize(pixels); }));
    m_connections.push_back(m_window->dpiChanged.connect([this](float) { resize(m_window->pixelSize()); }));
    m_connections.push_back(m_window->repaintRequested.connect([this] { m_fullRepaint = true; }));
    m_connections.push_back(m_window->closeRequested.connect([this] { m_open = false; }));
    m_connections.push_back(m_window->focusChanged.connect([this](bool focused) {
        if (!focused) {
            m_surface.dispatch(PointerEvent{PointerEvent::Type::Leave});
        }
    }));
}

UiWindow::~UiWindow() {
    m_connections.clear();
}

void UiWindow::resize(Vec2i pixels) {
    m_surface.setSize(m_window->logicalSize());
    auto canvas = Image::create(std::uint32_t(std::max(1, pixels.x)), std::uint32_t(std::max(1, pixels.y)),
                                AlphaMode::Premultiplied);
    if (canvas) {
        m_canvas = std::move(canvas).value();
    }
    m_fullRepaint = true;
}

void UiWindow::updateCursor() {
    const Cursor wanted = m_surface.cursor();
    if (wanted != m_cursor) {
        m_cursor = wanted;
        m_window->setCursor(wanted);
    }
}

bool UiWindow::frame() {
    m_surface.runTimers();
    updateCursor();
    m_surface.layout();
    const RectF damage = m_surface.takeDamage();
    if (!m_fullRepaint && damage.isEmpty()) {
        return false;
    }
    m_fullRepaint = false;
    // The whole surface is repainted; damage only decides whether to. (A
    // damage-clipped repaint is a later optimisation.)
    std::fill(m_canvas.pixels().begin(), m_canvas.pixels().end(), std::uint8_t(0));
    {
        RasterPaintBackend backend(m_canvas);
        Painter painter(backend);
        const float ratio = m_window->devicePixelRatio();
        painter.scale(ratio, ratio);
        m_surface.paint(painter);
    }
    m_window->present(m_canvas);
    ++m_frames;
    return true;
}

void runUntilClosed(UiWindow &window) {
    while (window.isOpen()) {
        window.frame();
        // Sleep until input, or until the surface's next timer is due.
        Duration wait = std::chrono::milliseconds(250);
        if (const std::optional<TimePoint> next = window.surface().nextTimer()) {
            wait = std::clamp(std::chrono::duration_cast<Duration>(*next - Clock::now()), Duration::zero(), wait);
        }
        processEvents(wait);
    }
}

} // namespace cfw
