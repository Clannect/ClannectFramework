#include "cfw/app/UiWindow.h"

#include <cstdlib>

#include <cmath>

#include "AccessibilityBridge.h"

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
    m_surface.movePointer = [this](Vec2 position) { m_window->setPointerPosition(position); };
    m_surface.writeClipboard = [](StringView text) { setClipboardText(text); };
    resize(m_window->pixelSize());
    m_connections.push_back(m_window->pointer.connect([this](const PointerEvent &e) {
        if (handleFrameEdge(e)) {
            return;
        }
        m_surface.dispatch(e);
        updateCursor();
    }));
    m_connections.push_back(m_window->key.connect([this](const KeyEvent &e) { m_surface.dispatch(e); }));
    m_connections.push_back(m_window->text.connect([this](const TextEvent &e) { m_surface.dispatch(e); }));
    m_window->setDropHandler([this](const DropEvent &e) { return m_surface.dispatch(e); });
    m_accessibility = detail::createAccessibilityBridge(*m_window, m_surface);
    m_connections.push_back(
        m_window->composition.connect([this](const CompositionEvent &e) { m_surface.dispatch(e); }));
    m_connections.push_back(m_window->resized.connect([this](Vec2i pixels) { resize(pixels); }));
    m_connections.push_back(m_window->dpiChanged.connect([this](float) { resize(m_window->pixelSize()); }));
    m_connections.push_back(m_window->repaintRequested.connect([this] { m_fullRepaint = true; }));
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

void UiWindow::beginMove() {
    endPress();
    m_window->beginSystemMove();
}

void UiWindow::beginResize(WindowEdge edge) {
    endPress();
    m_window->beginSystemResize(edge);
}

void UiWindow::toggleMaximized() { m_window->setMaximized(!m_window->isMaximized()); }

void UiWindow::endPress() {
    // The window manager has the pointer until the button comes up, and we
    // see no release: end the press so the element under it does not stay
    // pressed (and does not click).
    PointerEvent cancel;
    cancel.type = PointerEvent::Type::Cancel;
    m_surface.dispatch(cancel);
}

std::optional<WindowEdge> UiWindow::frameEdgeAt(Vec2 p) const {
    const Vec2 size = m_surface.size();
    const float b = kResizeBorder;
    const bool left = p.x < b;
    const bool right = p.x >= size.x - b;
    const bool top = p.y < b;
    const bool bottom = p.y >= size.y - b;
    if (top && left) return WindowEdge::TopLeft;
    if (top && right) return WindowEdge::TopRight;
    if (bottom && left) return WindowEdge::BottomLeft;
    if (bottom && right) return WindowEdge::BottomRight;
    if (left) return WindowEdge::Left;
    if (right) return WindowEdge::Right;
    if (top) return WindowEdge::Top;
    if (bottom) return WindowEdge::Bottom;
    return std::nullopt;
}

bool UiWindow::handleFrameEdge(const PointerEvent &e) {
    if (!m_window->isFrameless() || m_surface.pressed() || m_surface.popupCount() > 0) {
        return false;
    }
    if (e.type != PointerEvent::Type::Move && !(e.type == PointerEvent::Type::Press && e.button == PointerButton::Left)) {
        return false;
    }
    const std::optional<WindowEdge> edge = frameEdgeAt(e.position);
    if (!edge || m_window->isMaximized()) {
        if (m_onEdge) {
            m_onEdge = false;
            updateCursor();
        }
        return false;
    }
    if (e.type == PointerEvent::Type::Press) {
        m_onEdge = false;
        beginResize(*edge);
        return true;
    }
    static constexpr Cursor kShapes[] = {Cursor::SizeHorizontal, Cursor::SizeHorizontal, Cursor::SizeVertical,
                                         Cursor::SizeVertical,   Cursor::SizeDiagonal,   Cursor::SizeAntiDiagonal,
                                         Cursor::SizeAntiDiagonal, Cursor::SizeDiagonal}; // WindowEdge's order
    m_onEdge = true;
    m_cursor = kShapes[std::size_t(*edge)];
    m_window->setCursor(m_cursor);
    // The surface still sees the move (hover ends on the element below).
    PointerEvent leave = e;
    leave.type = PointerEvent::Type::Leave;
    m_surface.dispatch(leave);
    return true;
}

void UiWindow::updateCursor() {
    if (m_onEdge) {
        return;
    }
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
    if (m_accessibility) {
        m_accessibility->update();
    }
    // The input method follows the caret, and is off when no text is edited.
    const std::optional<RectF> textInput = m_surface.textInputArea();
    if (!m_textInputKnown || textInput != m_textInputArea) {
        m_textInputKnown = true;
        m_textInputArea = textInput;
        m_window->setTextInputArea(textInput);
    }
    const std::vector<RectF> damage = m_surface.takeDamageRects();
    if (!m_fullRepaint && damage.empty()) {
        return false;
    }
    const float ratio = m_window->devicePixelRatio();
    // Damaged areas in whole physical pixels (a pixel of margin for
    // antialiased edges). CFW_FULL_REPAINT=1 repaints everything each time.
    static const bool forceFull = [] {
        const char *v = std::getenv("CFW_FULL_REPAINT");
        return v && *v && *v != '0';
    }();
    const int cw = int(m_canvas.width()), ch = int(m_canvas.height());
    std::vector<Window::PixelRect> rects;
    if (!m_fullRepaint && !forceFull) {
        for (const RectF &d : damage) {
            const int x0 = std::max(0, int(std::floor(d.x * ratio)) - 1);
            const int y0 = std::max(0, int(std::floor(d.y * ratio)) - 1);
            const int x1 = std::min(cw, int(std::ceil((d.x + d.width) * ratio)) + 1);
            const int y1 = std::min(ch, int(std::ceil((d.y + d.height) * ratio)) + 1);
            if (x1 > x0 && y1 > y0) {
                rects.push_back({x0, y0, x1 - x0, y1 - y0});
            }
        }
    }
    const bool full = m_fullRepaint || forceFull || rects.empty();
    m_fullRepaint = false;
    if (full) {
        std::fill(m_canvas.pixels().begin(), m_canvas.pixels().end(), std::uint8_t(0));
        RasterPaintBackend backend(m_canvas);
        Painter painter(backend);
        painter.scale(ratio, ratio);
        m_surface.paint(painter);
    } else {
        for (const Window::PixelRect &r : rects) {
            for (int y = r.y; y < r.y + r.height; ++y) {
                std::uint8_t *row = m_canvas.row(std::uint32_t(y)).data();
                std::fill(row + std::size_t(r.x) * 4u, row + std::size_t(r.x + r.width) * 4u, std::uint8_t(0));
            }
            RasterPaintBackend backend(m_canvas);
            Painter painter(backend);
            painter.clipRect({float(r.x), float(r.y), float(r.width), float(r.height)});
            painter.scale(ratio, ratio);
            m_surface.paint(painter, {float(r.x) / ratio, float(r.y) / ratio, float(r.width) / ratio,
                                      float(r.height) / ratio});
        }
    }
    if (full) {
        m_window->present(m_canvas);
    } else {
        m_window->presentChanged(m_canvas, rects);
    }
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
