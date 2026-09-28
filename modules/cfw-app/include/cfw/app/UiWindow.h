#pragma once

// A native window showing a cfw-ui Surface: input from the window goes to
// the surface; the surface is painted on the CPU (cfw::Painter into an
// image) and presented whenever it has damage; the clipboard is the
// system's; the cursor becomes an I-beam over text fields.
//
//     auto window = UiWindow::create({"Clannect", {1280, 720}}, Theme::dark().withSystemFonts()).value();
//     window->surface().root().add<Button>("Play");
//     while (window->isOpen()) { window->frame(); processEvents(std::chrono::milliseconds(16)); }
//
// Idle when nothing changes: frame() paints only after input, a resize or
// an invalidation, so a static editor uses no CPU (spec §7).
//
// Threads: the window's thread.

#include <functional>
#include <memory>

#include "cfw/core/Result.h"
#include "cfw/image/Image.h"
#include "cfw/platform/Window.h"
#include "cfw/ui/Surface.h"

namespace cfw {

class UiWindow {
public:
    [[nodiscard]] static Result<std::unique_ptr<UiWindow>> create(const WindowOptions &options, Theme theme);
    ~UiWindow();
    UiWindow(const UiWindow &) = delete;
    UiWindow &operator=(const UiWindow &) = delete;

    [[nodiscard]] Surface &surface() noexcept { return m_surface; }
    [[nodiscard]] Window &window() noexcept { return *m_window; }
    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    void close() noexcept { m_open = false; }
    // What the window's close button does. Unset, it closes the window;
    // set, the handler decides (after asking about unsaved work, say) and
    // calls close() itself when the window should go.
    void setCloseHandler(std::function<void()> handler) { m_closeHandler = std::move(handler); }

    // Lays out, paints and presents if anything changed. Returns whether it
    // painted.
    bool frame();
    // Frames painted so far (idle behaviour is testable).
    [[nodiscard]] std::size_t framesPainted() const noexcept { return m_frames; }

private:
    UiWindow(std::unique_ptr<Window> window, Theme theme);
    void resize(Vec2i pixels);
    void updateCursor();

    std::unique_ptr<Window> m_window;
    Surface m_surface;
    Image m_canvas;
    std::vector<ScopedConnection> m_connections;
    bool m_open = true;
    std::function<void()> m_closeHandler;
    bool m_fullRepaint = true;
    std::size_t m_frames = 0;
    Cursor m_cursor = Cursor::Arrow;
};

// Runs until every given window is closed: events, then frames.
void runUntilClosed(UiWindow &window);

} // namespace cfw
