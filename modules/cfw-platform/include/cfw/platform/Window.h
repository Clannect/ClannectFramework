#pragma once

// Native top-level windows (spec §4.7): Win32 on Windows, X11 on Linux and
// other Unix desktops; macOS (Cocoa) is not written yet.
//
// A window reports input as cfw-core Input events (logical pixels), resizes,
// close requests, focus and DPI changes through signals, and shows pixels
// through present(): an RGBA image the size of the client area in physical
// pixels, painted by cfw::Painter on the CPU. (A GL context for the 3D
// viewport and a GPU paint backend come next; present() stays as the
// fallback and for tests.)
//
// Events are delivered by processEvents(), on the thread that created the
// windows. Nothing is delivered from inside present() or other calls.
//
// Not yet: IME composition beyond what the input method commits as text,
// X11 clipboard selections (the clipboard is in-process there), drag and
// drop, native dialogs, multiple monitors' work areas.
//
// Threads: windows and processEvents() on one thread; wakeUp() from any.

#include "cfw/core/Rect.h"
#include <optional>
#include <functional>
#include <cstdint>
#include <memory>

#include "cfw/core/Clock.h"
#include "cfw/core/Input.h"
#include "cfw/core/Result.h"
#include "cfw/core/Signal.h"
#include "cfw/core/String.h"
#include "cfw/core/Vec2.h"

namespace cfw {

class Image;


struct WindowOptions {
    String title = "Clannect";
    Vec2i size{1280, 720}; // client area, logical pixels
    bool resizable = true;
    bool visible = true;
};

class Window {
public:
    // Fails with Unsupported where no window system is available (a
    // headless server, or a platform without a backend).
    [[nodiscard]] static Result<std::unique_ptr<Window>> create(const WindowOptions &options = {});
    virtual ~Window();
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    virtual void show() = 0;
    virtual void hide() = 0;
    virtual void setTitle(StringView title) = 0;
    // Client area in logical pixels.
    virtual void setSize(Vec2i size) = 0;
    // Client area in physical pixels (what present() expects).
    [[nodiscard]] virtual Vec2i pixelSize() const = 0;
    // Physical pixels per logical pixel (1 at 96 dpi, 1.5 at 144...).
    [[nodiscard]] virtual float devicePixelRatio() const = 0;
    [[nodiscard]] Vec2 logicalSize() const;

    // Shows `image` (straight or premultiplied RGBA, drawn over black) at
    // the client area's top-left. Kept, so the OS can repaint from it.
    virtual void present(const Image &image) = 0;
    // What the window system shows for the client area now (screenshots and
    // tests).
    [[nodiscard]] virtual Result<Image> capture() const = 0;
    // Asks for a repaintRequested signal from the next processEvents().
    virtual void requestRepaint() = 0;
    virtual void setCursor(Cursor cursor) = 0;
    // Moves the pointer to `position` (logical pixels in the client area), as
    // a camera that turns with the mouse does to keep it from reaching the
    // screen's edge. The move arrives as an ordinary pointer event. Does
    // nothing where the platform cannot.
    virtual void setPointerPosition(Vec2 position) { (void)position; }
    // Covers the whole monitor the window is on, without a frame (the
    // window manager's full-screen state on X11), or puts it back as it was.
    // Does nothing where the platform cannot.
    virtual void setFullScreen(bool fullScreen) { (void)fullScreen; }
    [[nodiscard]] virtual bool isFullScreen() const { return false; }
    // Where the text caret is (logical pixels), so an input method places its
    // candidate window beside it; nothing when no text is being edited, which
    // turns the input method off (keys then reach the application as keys).
    virtual void setTextInputArea(const std::optional<RectF> &caret) { (void)caret; }
    // Where the client area's top-left is on the screen, in physical pixels
    // (what screen readers are told; nothing known: the origin).
    [[nodiscard]] virtual Vec2i screenPosition() const { return {}; }
    // HWND on Windows, the X11 Window id on X11 (for embedding WebView2 and
    // for GL context creation).
    [[nodiscard]] virtual void *nativeHandle() const = 0;

    Signal<const PointerEvent &> pointer;
    Signal<const KeyEvent &> key;
    Signal<const TextEvent &> text;
    Signal<const CompositionEvent &> composition;
    Signal<Vec2i> resized; // physical pixels
    Signal<float> dpiChanged;
    Signal<bool> focusChanged;
    Signal<> repaintRequested;
    Signal<> closeRequested; // the window stays open until it is destroyed

    // Files dragged over the window: the handler says whether they would be
    // taken where the pointer is (Enter, Move) and takes them (Drop). No
    // handler: the window refuses drops.
    void setDropHandler(std::function<bool(const DropEvent &)> handler) { m_dropHandler = std::move(handler); }

    // Windows only: sees each message before the window does; a value
    // handles it (the accessibility bridge answers WM_GETOBJECT this way).
    std::function<std::optional<std::intptr_t>(unsigned message, std::uintptr_t wParam, std::intptr_t lParam)>
        nativeMessageFilter;

protected:
    Window() = default;
    bool handleDrop(const DropEvent &event) { return m_dropHandler && m_dropHandler(event); }

private:
    std::function<bool(const DropEvent &)> m_dropHandler;
};

// Delivers the OS events waiting for every window, first waiting up to
// `maxWait` for one to arrive (or for wakeUp()). Returns false when there is
// no window system.
bool processEvents(Duration maxWait);
// Makes a waiting processEvents() return. Any thread.
void wakeUp();

// The system clipboard's text (UTF-8), or empty.
[[nodiscard]] String clipboardText();
void setClipboardText(StringView text);

} // namespace cfw
