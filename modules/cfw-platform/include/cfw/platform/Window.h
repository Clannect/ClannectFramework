#pragma once

// Native top-level windows: Win32 on Windows, X11 on Linux and
// other Unix desktops, AppKit on macOS (decision 0016: not yet compiled
// or run on a Mac; CI's macos job is its first check).
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
// Pointers arrive on two signals. `pointer` is the mouse, and whichever
// finger or pen is acting as the mouse (the primary contact): code that only
// knows the mouse works with touch through it. `touch` is every finger and
// pen, each with its own id, for following several at once. A primary
// contact is reported on both, so a handler uses one signal or the other.
//
// Keys carry both the key the layout produces and the key's position (see
// KeyEvent in cfw/core/Input.h). Gamepads are separate: cfw/platform/Gamepad.h.
//
// Not yet: multiple monitors' work areas.
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
    // Moves the pointer to `position` (logical pixels in the client area).
    // The move arrives as an ordinary pointer event. Does nothing where the
    // platform cannot. A camera that turns with the mouse should not use
    // this to re-centre the pointer after every move (fast movement is lost
    // at the screen's edge, and tablets and remote desktops report absolute
    // positions that warping fights): use setRelativeMouse() instead.
    virtual void setPointerPosition(Vec2 position) { (void)position; }

    // Relative mouse mode ("pointer lock"), for first-person cameras. While
    // it is on the cursor is hidden and cannot leave the window, and every
    // mouse movement arrives on `pointer` as a Move whose `delta` is the
    // mouse's own movement: raw device counts, not accelerated and not
    // stopped by the screen's edge. `position` stays where the pointer was
    // when the mode began (so nothing under it changes), and the pointer is
    // there again when the mode ends. Buttons and the wheel work as usual.
    //
    // The mode switches itself off when the window loses the keyboard focus
    // (Alt+Tab, a system dialog) and says so through relativeMouseChanged;
    // it does not come back by itself when the focus does. Turning it on
    // fails silently where the platform cannot do it or the window is not
    // focused: ask isRelativeMouse().
    virtual void setRelativeMouse(bool relative) { (void)relative; }
    [[nodiscard]] virtual bool isRelativeMouse() const { return false; }
    // Keeps the (visible) cursor inside the client area, as a strategy
    // game's map scrolling at the edges wants. Lifted while the window does
    // not have the focus and restored when it has it again; switched off
    // only by setCursorConfined(false). Independent of relative mode.
    virtual void setCursorConfined(bool confined) { (void)confined; }
    [[nodiscard]] virtual bool isCursorConfined() const { return false; }
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
    // HWND on Windows, the X11 Window id on X11, the content NSView on macOS
    // (for embedding WebView2 and for GL context creation).
    [[nodiscard]] virtual void *nativeHandle() const = 0;

    Signal<const PointerEvent &> pointer;
    // Every finger and pen on the window, the primary one included, each
    // with its own id: Press, Move, Release, or Cancel when the system takes
    // the contact away. A pen hovering sends Moves with no pressure.
    Signal<const PointerEvent &> touch;
    // Relative mouse mode turned on (true) or off, by setRelativeMouse() or
    // by the window losing the focus.
    Signal<bool> relativeMouseChanged;
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

    // Delivers one touch or pen contact as the platform backends do: to
    // `touch`, and, when it is the primary contact, to `pointer` as the mouse
    // it stands in for (the left button; a second press close to the first
    // and soon after it is a double click). `event.kind` must be Touch or
    // Pen. For tests, and for an embedder with its own source of touches.
    void deliverTouch(PointerEvent event);

    // Windows only: sees each message before the window does; a value
    // handles it (the accessibility bridge answers WM_GETOBJECT this way).
    std::function<std::optional<std::intptr_t>(unsigned message, std::uintptr_t wParam, std::intptr_t lParam)>
        nativeMessageFilter;

protected:
    Window() = default;
    bool handleDrop(const DropEvent &event) { return m_dropHandler && m_dropHandler(event); }

private:
    std::function<bool(const DropEvent &)> m_dropHandler;
    // Double-tap detection for the primary contact.
    TimePoint m_lastTapTime{};
    Vec2 m_lastTapAt;
    int m_tapCount = 0;
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
