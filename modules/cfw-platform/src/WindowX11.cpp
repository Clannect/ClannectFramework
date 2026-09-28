// X11 backend: Xlib, an input method for text (Xutf8LookupString), MIT
// XPutImage presentation, Xft.dpi for the scale.

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "PixelCopy.h"
#include "X11Internal.h"
#include "cfw/image/Image.h"
#include "cfw/platform/Window.h"

// Xlib defines None, Bool, Status... as macros: CFW names that collide are
// captured before it is included.
namespace cfw::x11 {
constexpr Modifier kNoModifier = Modifier::None;
constexpr PointerButton kNoButton = PointerButton::None;
} // namespace cfw::x11

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

namespace cfw {

namespace {

class WindowX11;

struct X11Connection {
    Display *display = nullptr;
    XIM im = nullptr;
    Atom deleteWindow = 0;
    Atom netWmName = 0;
    Atom utf8String = 0;
    int wakePipe[2] = {-1, -1};
    std::map<::Window, WindowX11 *> windows;
    String clipboard; // in-process until X selections are implemented
};

X11Connection &connection() {
    static X11Connection c;
    return c;
}

bool connect() {
    X11Connection &c = connection();
    if (c.display) {
        return true;
    }
    c.display = XOpenDisplay(nullptr);
    if (!c.display) {
        return false;
    }
    XrmInitialize();
    c.deleteWindow = XInternAtom(c.display, "WM_DELETE_WINDOW", False);
    c.netWmName = XInternAtom(c.display, "_NET_WM_NAME", False);
    c.utf8String = XInternAtom(c.display, "UTF8_STRING", False);
    c.im = XOpenIM(c.display, nullptr, nullptr, nullptr);
    if (::pipe(c.wakePipe) != 0) {
        c.wakePipe[0] = c.wakePipe[1] = -1;
    }
    return true;
}

float systemScale(Display *display) {
    const char *resources = XResourceManagerString(display);
    if (!resources) {
        return 1.0f;
    }
    XrmDatabase db = XrmGetStringDatabase(resources);
    char *type = nullptr;
    XrmValue value{};
    float scale = 1.0f;
    if (db && XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &value) && value.addr) {
        const float dpi = float(std::atof(value.addr));
        if (dpi > 0.0f) {
            scale = std::round(dpi / 96.0f * 4.0f) / 4.0f; // quarter steps
        }
    }
    if (db) {
        XrmDestroyDatabase(db);
    }
    return std::max(1.0f, scale);
}

Key keyOf(KeySym sym) {
    if (sym >= XK_a && sym <= XK_z) {
        return static_cast<Key>(static_cast<unsigned>(Key::A) + (sym - XK_a));
    }
    if (sym >= XK_A && sym <= XK_Z) {
        return static_cast<Key>(static_cast<unsigned>(Key::A) + (sym - XK_A));
    }
    if (sym >= XK_0 && sym <= XK_9) {
        return static_cast<Key>(static_cast<unsigned>(Key::Digit0) + (sym - XK_0));
    }
    if (sym >= XK_F1 && sym <= XK_F12) {
        return static_cast<Key>(static_cast<unsigned>(Key::F1) + (sym - XK_F1));
    }
    switch (sym) {
    case XK_Tab: case XK_ISO_Left_Tab: return Key::Tab;
    case XK_Return: case XK_KP_Enter: return Key::Enter;
    case XK_Escape: return Key::Escape;
    case XK_space: return Key::Space;
    case XK_BackSpace: return Key::Backspace;
    case XK_Delete: case XK_KP_Delete: return Key::Delete;
    case XK_Left: case XK_KP_Left: return Key::Left;
    case XK_Right: case XK_KP_Right: return Key::Right;
    case XK_Up: case XK_KP_Up: return Key::Up;
    case XK_Down: case XK_KP_Down: return Key::Down;
    case XK_Home: case XK_KP_Home: return Key::Home;
    case XK_End: case XK_KP_End: return Key::End;
    case XK_Page_Up: case XK_KP_Page_Up: return Key::PageUp;
    case XK_Page_Down: case XK_KP_Page_Down: return Key::PageDown;
    case XK_grave: case XK_asciitilde: return Key::Backquote;
    case XK_minus: case XK_underscore: return Key::Minus;
    case XK_equal: case XK_plus: return Key::Equal;
    case XK_bracketleft: case XK_braceleft: return Key::BracketLeft;
    case XK_bracketright: case XK_braceright: return Key::BracketRight;
    case XK_backslash: case XK_bar: return Key::Backslash;
    case XK_semicolon: case XK_colon: return Key::Semicolon;
    case XK_apostrophe: case XK_quotedbl: return Key::Quote;
    case XK_comma: case XK_less: return Key::Comma;
    case XK_period: case XK_greater: return Key::Period;
    case XK_slash: case XK_question: return Key::Slash;
    case XK_Insert: case XK_KP_Insert: return Key::Insert;
    case XK_Shift_L: case XK_Shift_R: return Key::Shift;
    case XK_Control_L: case XK_Control_R: return Key::Control;
    case XK_Alt_L: case XK_Alt_R: return Key::Alt;
    case XK_Super_L: case XK_Super_R: return Key::Meta;
    default: return Key::Unknown;
    }
}

Modifier modifiersOf(unsigned state) {
    Modifier m = x11::kNoModifier;
    if (state & ShiftMask) m = m | Modifier::Shift;
    if (state & ControlMask) m = m | Modifier::Control;
    if (state & Mod1Mask) m = m | Modifier::Alt;
    if (state & Mod4Mask) m = m | Modifier::Meta;
    return m;
}

unsigned cursorShape(Cursor cursor) {
    switch (cursor) {
    case Cursor::IBeam: return XC_xterm;
    case Cursor::Hand: return XC_hand2;
    case Cursor::Wait: return XC_watch;
    case Cursor::Crosshair: return XC_crosshair;
    case Cursor::SizeHorizontal: return XC_sb_h_double_arrow;
    case Cursor::SizeVertical: return XC_sb_v_double_arrow;
    case Cursor::SizeDiagonal: return XC_bottom_right_corner;
    case Cursor::SizeAntiDiagonal: return XC_bottom_left_corner;
    case Cursor::SizeAll: return XC_fleur;
    case Cursor::NotAllowed: return XC_X_cursor;
    default: return XC_left_ptr;
    }
}

class WindowX11 final : public Window {
public:
    WindowX11(::Window window, XIC ic, float scale, Vec2i size) : m_window(window), m_ic(ic), m_scale(scale), m_size(size) {
        connection().windows[window] = this;
    }

    ~WindowX11() override {
        X11Connection &c = connection();
        c.windows.erase(m_window);
        if (m_ic) {
            XDestroyIC(m_ic);
        }
        for (auto &[shape, cursor] : m_cursors) {
            XFreeCursor(c.display, cursor);
        }
        XDestroyWindow(c.display, m_window);
        XFlush(c.display);
    }

    void show() override {
        XMapWindow(connection().display, m_window);
        XFlush(connection().display);
    }
    void hide() override {
        XUnmapWindow(connection().display, m_window);
        XFlush(connection().display);
    }

    void setTitle(StringView title) override {
        X11Connection &c = connection();
        const String text(title);
        XStoreName(c.display, m_window, text.c_str());
        XChangeProperty(c.display, m_window, c.netWmName, c.utf8String, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(text.data()), int(text.size()));
        XFlush(c.display);
    }

    void setSize(Vec2i size) override {
        XResizeWindow(connection().display, m_window, unsigned(std::max(1, int(float(size.x) * m_scale))),
                      unsigned(std::max(1, int(float(size.y) * m_scale))));
        XFlush(connection().display);
    }

    Vec2i pixelSize() const override { return m_size; }
    float devicePixelRatio() const override { return m_scale; }

    void present(const Image &image) override {
        detail::toBgrx(image, m_frame);
        m_frameSize = {int(image.width()), int(image.height())};
        blit();
    }

    Result<Image> capture() const override {
        Display *display = connection().display;
        XSync(display, False);
        XImage *shot = XGetImage(display, m_window, 0, 0, unsigned(m_size.x), unsigned(m_size.y), AllPlanes, ZPixmap);
        if (!shot) {
            return Error(ErrorCode::IoError, "XGetImage failed");
        }
        auto created = Image::create(std::uint32_t(m_size.x), std::uint32_t(m_size.y));
        if (!created) {
            XDestroyImage(shot);
            return created;
        }
        Image image = std::move(created).value();
        for (int y = 0; y < m_size.y; ++y) {
            std::uint8_t *row = image.row(std::uint32_t(y)).data();
            for (int x = 0; x < m_size.x; ++x) {
                const unsigned long p = XGetPixel(shot, x, y);
                row[x * 4] = std::uint8_t(p >> 16);
                row[x * 4 + 1] = std::uint8_t(p >> 8);
                row[x * 4 + 2] = std::uint8_t(p);
                row[x * 4 + 3] = 255;
            }
        }
        XDestroyImage(shot);
        return image;
    }

    void requestRepaint() override { m_repaint = true; }

    void setCursor(Cursor cursor) override {
        Display *display = connection().display;
        if (cursor == Cursor::Hidden) {
            static const char empty[1] = {0};
            const Pixmap blank = XCreateBitmapFromData(display, m_window, empty, 1, 1);
            XColor black{};
            const ::Cursor invisible = XCreatePixmapCursor(display, blank, blank, &black, &black, 0, 0);
            XFreePixmap(display, blank);
            XDefineCursor(display, m_window, invisible);
            m_cursors.emplace(~0u, invisible);
            return;
        }
        const unsigned shape = cursorShape(cursor);
        auto it = m_cursors.find(shape);
        if (it == m_cursors.end()) {
            it = m_cursors.emplace(shape, XCreateFontCursor(display, shape)).first;
        }
        XDefineCursor(display, m_window, it->second);
    }

    void *nativeHandle() const override { return reinterpret_cast<void *>(m_window); }

    // ---- Event handling (called by processEvents) ----
    void handle(XEvent &event);
    [[nodiscard]] bool repaintPending() const noexcept { return m_repaint; }
    bool takeRepaint() {
        const bool repaint = m_repaint;
        m_repaint = false;
        return repaint;
    }

private:
    void blit() {
        if (m_frame.empty()) {
            return;
        }
        Display *display = connection().display;
        XImage image{};
        image.width = m_frameSize.x;
        image.height = m_frameSize.y;
        image.format = ZPixmap;
        image.data = reinterpret_cast<char *>(m_frame.data());
        image.byte_order = LSBFirst;
        image.bitmap_unit = 32;
        image.bitmap_bit_order = LSBFirst;
        image.bitmap_pad = 32;
        image.depth = DefaultDepth(display, DefaultScreen(display));
        image.bytes_per_line = m_frameSize.x * 4;
        image.bits_per_pixel = 32;
        image.red_mask = 0xFF0000;
        image.green_mask = 0x00FF00;
        image.blue_mask = 0x0000FF;
        XInitImage(&image);
        XPutImage(display, m_window, DefaultGC(display, DefaultScreen(display)), &image, 0, 0, 0, 0,
                  unsigned(std::min(m_frameSize.x, m_size.x)), unsigned(std::min(m_frameSize.y, m_size.y)));
        XFlush(display);
    }

    Vec2 logical(int x, int y) const { return {float(x) / m_scale, float(y) / m_scale}; }

    ::Window m_window;
    XIC m_ic;
    float m_scale;
    Vec2i m_size;
    std::vector<std::uint32_t> m_frame;
    Vec2i m_frameSize;
    bool m_repaint = false;
    std::map<unsigned, ::Cursor> m_cursors;
    // Double-click detection.
    Time m_lastPressTime = 0;
    unsigned m_lastButton = 0;
    Vec2 m_lastPressAt;
    int m_clicks = 0;
};

void WindowX11::handle(XEvent &event) {
    Display *display = connection().display;
    switch (event.type) {
    case ConfigureNotify: {
        const Vec2i size{event.xconfigure.width, event.xconfigure.height};
        if (!(size == m_size)) {
            m_size = size;
            resized.emit(size);
            m_repaint = true;
        }
        break;
    }
    case Expose:
        if (event.xexpose.count == 0) {
            blit();
            m_repaint = true;
        }
        break;
    case ClientMessage:
        if (Atom(event.xclient.data.l[0]) == connection().deleteWindow) {
            closeRequested.emit();
        }
        break;
    case FocusIn:
        if (m_ic) XSetICFocus(m_ic);
        focusChanged.emit(true);
        break;
    case FocusOut:
        if (m_ic) XUnsetICFocus(m_ic);
        focusChanged.emit(false);
        break;
    case MotionNotify: {
        PointerEvent e;
        e.type = PointerEvent::Type::Move;
        e.position = logical(event.xmotion.x, event.xmotion.y);
        e.modifiers = modifiersOf(event.xmotion.state);
        pointer.emit(e);
        break;
    }
    case LeaveNotify: {
        PointerEvent e;
        e.type = PointerEvent::Type::Leave;
        e.position = logical(event.xcrossing.x, event.xcrossing.y);
        pointer.emit(e);
        break;
    }
    case ButtonPress:
    case ButtonRelease: {
        const XButtonEvent &b = event.xbutton;
        PointerEvent e;
        e.position = logical(b.x, b.y);
        e.modifiers = modifiersOf(b.state);
        if (b.button >= 4 && b.button <= 7) {
            if (event.type == ButtonPress) {
                e.type = PointerEvent::Type::Wheel;
                constexpr float kNotch = 48.0f; // three lines of 16 px
                e.wheelDelta = b.button == 4 ? Vec2{0, kNotch} : b.button == 5 ? Vec2{0, -kNotch}
                             : b.button == 6 ? Vec2{kNotch, 0} : Vec2{-kNotch, 0};
                pointer.emit(e);
            }
            break;
        }
        e.button = b.button == 1 ? PointerButton::Left : b.button == 3 ? PointerButton::Right : PointerButton::Middle;
        if (event.type == ButtonPress) {
            e.type = PointerEvent::Type::Press;
            const Vec2 d = e.position - m_lastPressAt;
            const bool again = b.button == m_lastButton && b.time - m_lastPressTime <= 400 && std::abs(d.x) <= 4 &&
                               std::abs(d.y) <= 4;
            m_clicks = again ? m_clicks + 1 : 1;
            m_lastPressTime = b.time;
            m_lastButton = b.button;
            m_lastPressAt = e.position;
            e.clickCount = m_clicks;
        } else {
            e.type = PointerEvent::Type::Release;
        }
        pointer.emit(e);
        break;
    }
    case KeyPress:
    case KeyRelease: {
        XKeyEvent &k = event.xkey;
        KeyEvent e;
        e.type = event.type == KeyPress ? KeyEvent::Type::Press : KeyEvent::Type::Release;
        e.modifiers = modifiersOf(k.state);
        char buffer[64] = {};
        KeySym sym = NoSymbol;
        int length = 0;
        if (event.type == KeyPress && m_ic) {
            Status status = 0;
            length = Xutf8LookupString(m_ic, &k, buffer, int(sizeof buffer) - 1, &sym, &status);
            if (status != XLookupChars && status != XLookupBoth) {
                length = 0;
            }
            if (status != XLookupKeySym && status != XLookupBoth) {
                sym = XLookupKeysym(&k, 0);
            }
        } else {
            sym = XLookupKeysym(&k, 0);
        }
        e.key = keyOf(sym);
        if (event.type == KeyRelease && XEventsQueued(display, QueuedAfterReading)) {
            // Auto-repeat arrives as release+press with the same time.
            XEvent next;
            XPeekEvent(display, &next);
            if (next.type == KeyPress && next.xkey.time == k.time && next.xkey.keycode == k.keycode) {
                XNextEvent(display, &next);
                e.type = KeyEvent::Type::Press;
                e.repeat = true;
                key.emit(e);
                break;
            }
        }
        key.emit(e);
        if (length > 0 && !(e.modifiers == Modifier::Control)) {
            text.emit(TextEvent{String(buffer, std::size_t(length))});
        }
        break;
    }
    default:
        break;
    }
}

} // namespace

Result<std::unique_ptr<Window>> Window::create(const WindowOptions &options) {
    if (!connect()) {
        return Error(ErrorCode::Unsupported, "no X display");
    }
    X11Connection &c = connection();
    Display *display = c.display;
    const int screen = DefaultScreen(display);
    const float scale = systemScale(display);
    const Vec2i size{std::max(1, int(float(options.size.x) * scale)), std::max(1, int(float(options.size.y) * scale))};
    const ::Window window = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, unsigned(size.x),
                                                unsigned(size.y), 0, BlackPixel(display, screen),
                                                BlackPixel(display, screen));
    XSelectInput(display, window,
                 ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                     ButtonReleaseMask | PointerMotionMask | LeaveWindowMask | FocusChangeMask);
    Atom protocols[] = {c.deleteWindow};
    XSetWMProtocols(display, window, protocols, 1);
    if (!options.resizable) {
        XSizeHints *hints = XAllocSizeHints();
        hints->flags = PMinSize | PMaxSize;
        hints->min_width = hints->max_width = size.x;
        hints->min_height = hints->max_height = size.y;
        XSetWMNormalHints(display, window, hints);
        XFree(hints);
    }
    XIC ic = c.im ? XCreateIC(c.im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, window,
                              XNFocusWindow, window, nullptr)
                  : nullptr;
    auto result = std::make_unique<WindowX11>(window, ic, scale, size);
    result->setTitle(options.title);
    if (options.visible) {
        result->show();
    }
    return std::unique_ptr<Window>(std::move(result));
}

void *detail::x11Display() { return connection().display; }

bool processEvents(Duration maxWait) {
    X11Connection &c = connection();
    if (!c.display) {
        return false;
    }
    Display *display = c.display;
    XFlush(display);
    const bool repaintPending = std::any_of(c.windows.begin(), c.windows.end(),
                                            [](const auto &entry) { return entry.second->repaintPending(); });
    if (!XPending(display) && !repaintPending) {
        pollfd fds[2] = {{ConnectionNumber(display), POLLIN, 0}, {c.wakePipe[0], POLLIN, 0}};
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(maxWait).count();
        ::poll(fds, c.wakePipe[0] >= 0 ? 2 : 1, int(std::clamp<long long>(ms, 0, 1 << 30)));
        if (c.wakePipe[0] >= 0 && (fds[1].revents & POLLIN)) {
            char drain[64];
            [[maybe_unused]] const auto n = ::read(c.wakePipe[0], drain, sizeof drain);
        }
    }
    while (XPending(display)) {
        XEvent event;
        XNextEvent(display, &event);
        if (XFilterEvent(&event, None)) {
            continue; // consumed by the input method
        }
        const auto it = c.windows.find(event.xany.window);
        if (it != c.windows.end()) {
            it->second->handle(event);
        }
    }
    // Repaints last, once per window, after the input that caused them. A
    // handler may close windows, so each is looked up again.
    std::vector<::Window> ids;
    for (const auto &entry : c.windows) {
        ids.push_back(entry.first);
    }
    for (const ::Window id : ids) {
        const auto it = c.windows.find(id);
        if (it != c.windows.end() && it->second->takeRepaint()) {
            it->second->repaintRequested.emit();
        }
    }
    return true;
}

void wakeUp() {
    const int fd = connection().wakePipe[1];
    if (fd >= 0) {
        const char byte = 1;
        [[maybe_unused]] const auto n = ::write(fd, &byte, 1);
    }
}

String clipboardText() { return connection().clipboard; }
void setClipboardText(StringView text) { connection().clipboard = String(text); }

} // namespace cfw
