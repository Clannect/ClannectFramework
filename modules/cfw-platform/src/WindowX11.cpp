// X11 backend: Xlib, an input method for text (Xutf8LookupString), MIT
// XPutImage presentation, Xft.dpi for the scale, and the CLIPBOARD
// selection (ICCCM: TARGETS, UTF8_STRING, STRING; INCR when reading), and
// file drops (XDND version 5, text/uri-list).

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <vector>

#include "PixelCopy.h"
#include "X11Internal.h"
#include "cfw/core/Url.h"
#include "cfw/image/Image.h"
#include "cfw/platform/Window.h"

// Xlib defines None, Bool, Status... as macros: CFW names that collide are
// captured before it is included.
namespace cfw::x11 {
constexpr Modifier kNoModifier = Modifier::None;
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
    // The clipboard: an unmapped window owns the CLIPBOARD selection while
    // `clipboard` holds what this process copied.
    ::Window selectionWindow = 0;
    Atom clipboardAtom = 0;
    Atom targets = 0;
    Atom textAtom = 0;
    Atom incr = 0;
    Atom transferProperty = 0;
    // XDND.
    Atom xdndAware = 0, xdndEnter = 0, xdndPosition = 0, xdndStatus = 0, xdndLeave = 0, xdndDrop = 0,
         xdndFinished = 0, xdndSelection = 0, xdndTypeList = 0, xdndActionCopy = 0, uriList = 0, dropProperty = 0;
    String clipboard;
    bool ownsClipboard = false;
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
    c.clipboardAtom = XInternAtom(c.display, "CLIPBOARD", False);
    c.targets = XInternAtom(c.display, "TARGETS", False);
    c.textAtom = XInternAtom(c.display, "TEXT", False);
    c.incr = XInternAtom(c.display, "INCR", False);
    c.transferProperty = XInternAtom(c.display, "CFW_SELECTION", False);
    c.xdndAware = XInternAtom(c.display, "XdndAware", False);
    c.xdndEnter = XInternAtom(c.display, "XdndEnter", False);
    c.xdndPosition = XInternAtom(c.display, "XdndPosition", False);
    c.xdndStatus = XInternAtom(c.display, "XdndStatus", False);
    c.xdndLeave = XInternAtom(c.display, "XdndLeave", False);
    c.xdndDrop = XInternAtom(c.display, "XdndDrop", False);
    c.xdndFinished = XInternAtom(c.display, "XdndFinished", False);
    c.xdndSelection = XInternAtom(c.display, "XdndSelection", False);
    c.xdndTypeList = XInternAtom(c.display, "XdndTypeList", False);
    c.xdndActionCopy = XInternAtom(c.display, "XdndActionCopy", False);
    c.uriList = XInternAtom(c.display, "text/uri-list", False);
    c.dropProperty = XInternAtom(c.display, "CFW_DROP", False);
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

    // EWMH: _NET_WM_STATE_FULLSCREEN, asked of the window manager for a
    // mapped window and set as the initial state for an unmapped one.
    void setFullScreen(bool fullScreen) override {
        X11Connection &c = connection();
        const Atom state = XInternAtom(c.display, "_NET_WM_STATE", False);
        const Atom fullScreenAtom = XInternAtom(c.display, "_NET_WM_STATE_FULLSCREEN", False);
        XWindowAttributes attributes{};
        XGetWindowAttributes(c.display, m_window, &attributes);
        if (attributes.map_state == IsViewable) {
            XEvent event{};
            event.xclient.type = ClientMessage;
            event.xclient.window = m_window;
            event.xclient.message_type = state;
            event.xclient.format = 32;
            event.xclient.data.l[0] = fullScreen ? 1 : 0; // _NET_WM_STATE_ADD / _REMOVE
            event.xclient.data.l[1] = long(fullScreenAtom);
            event.xclient.data.l[3] = 1; // a normal application
            XSendEvent(c.display, DefaultRootWindow(c.display), False,
                       SubstructureRedirectMask | SubstructureNotifyMask, &event);
        } else if (fullScreen) {
            XChangeProperty(c.display, m_window, state, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char *>(&fullScreenAtom), 1);
        } else {
            XDeleteProperty(c.display, m_window, state);
        }
        XFlush(c.display);
        m_fullScreen = fullScreen;
    }
    bool isFullScreen() const override { return m_fullScreen; }

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

    void setPointerPosition(Vec2 position) override {
        Display *display = connection().display;
        XWarpPointer(display, None, m_window, 0, 0, 0, 0, int(std::lround(position.x * m_scale)),
                     int(std::lround(position.y * m_scale)));
        XFlush(display);
    }

    void *nativeHandle() const override { return reinterpret_cast<void *>(m_window); }

    // ---- Event handling (called by processEvents) ----
    void handle(XEvent &event);
    void handleXdnd(const XClientMessageEvent &message);
    void finishDrop(bool converted);
    void sendXdnd(::Window to, Atom type, long l1, long l2, long l3, long l4);
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
    bool m_fullScreen = false;
    std::map<unsigned, ::Cursor> m_cursors;
    // The drag over the window (XDND), if any.
    ::Window m_dndSource = 0;
    bool m_dndOffersFiles = false;
    bool m_dndEntered = false;
    bool m_dndAccepted = false;
    Vec2 m_dndPosition;
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
        if (event.xclient.message_type == connection().xdndEnter ||
            event.xclient.message_type == connection().xdndPosition ||
            event.xclient.message_type == connection().xdndLeave ||
            event.xclient.message_type == connection().xdndDrop) {
            handleXdnd(event.xclient);
        } else if (Atom(event.xclient.data.l[0]) == connection().deleteWindow) {
            closeRequested.emit();
        }
        break;
    case SelectionNotify:
        if (event.xselection.selection == connection().xdndSelection) {
            finishDrop(event.xselection.property != 0);
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

void WindowX11::sendXdnd(::Window to, Atom type, long l1, long l2, long l3, long l4) {
    Display *display = connection().display;
    XEvent reply{};
    reply.xclient.type = ClientMessage;
    reply.xclient.display = display;
    reply.xclient.window = to;
    reply.xclient.message_type = type;
    reply.xclient.format = 32;
    reply.xclient.data.l[0] = long(m_window);
    reply.xclient.data.l[1] = l1;
    reply.xclient.data.l[2] = l2;
    reply.xclient.data.l[3] = l3;
    reply.xclient.data.l[4] = l4;
    XSendEvent(display, to, False, NoEventMask, &reply);
    XFlush(display);
}

void WindowX11::handleXdnd(const XClientMessageEvent &message) {
    X11Connection &c = connection();
    Display *display = c.display;
    const auto source = ::Window(message.data.l[0]);
    if (message.message_type == c.xdndEnter) {
        m_dndSource = source;
        m_dndEntered = false;
        m_dndAccepted = false;
        m_dndOffersFiles = false;
        if (message.data.l[1] & 1) {
            // More than three types: the source lists them in a property.
            Atom type = 0;
            int format = 0;
            unsigned long items = 0, remaining = 0;
            unsigned char *data = nullptr;
            if (XGetWindowProperty(display, source, c.xdndTypeList, 0, 1024, False, XA_ATOM, &type, &format, &items,
                                   &remaining, &data) == Success &&
                data) {
                const auto *types = reinterpret_cast<const unsigned long *>(data);
                for (unsigned long i = 0; i < items; ++i) {
                    m_dndOffersFiles = m_dndOffersFiles || Atom(types[i]) == c.uriList;
                }
                XFree(data);
            }
        } else {
            for (int i = 2; i <= 4; ++i) {
                m_dndOffersFiles = m_dndOffersFiles || Atom(message.data.l[i]) == c.uriList;
            }
        }
        return;
    }
    if (source != m_dndSource) {
        return; // not the drag we are following
    }
    if (message.message_type == c.xdndPosition) {
        // Root coordinates, packed x << 16 | y.
        const int rootX = int((message.data.l[2] >> 16) & 0xFFFF);
        const int rootY = int(message.data.l[2] & 0xFFFF);
        int x = 0, y = 0;
        ::Window child = 0;
        XTranslateCoordinates(display, DefaultRootWindow(display), m_window, rootX, rootY, &x, &y, &child);
        m_dndPosition = logical(x, y);
        DropEvent drag;
        drag.type = m_dndEntered ? DropEvent::Type::Move : DropEvent::Type::Enter;
        drag.position = m_dndPosition;
        m_dndEntered = true;
        m_dndAccepted = m_dndOffersFiles && handleDrop(drag);
        // Accepted or not, keep sending positions (bit 1): the answer
        // changes as the pointer crosses elements.
        sendXdnd(m_dndSource, c.xdndStatus, (m_dndAccepted ? 1 : 0) | 2, 0, 0,
                 m_dndAccepted ? long(c.xdndActionCopy) : 0);
    } else if (message.message_type == c.xdndLeave) {
        if (m_dndEntered) {
            DropEvent leave;
            leave.type = DropEvent::Type::Leave;
            leave.position = m_dndPosition;
            handleDrop(leave);
        }
        m_dndSource = 0;
        m_dndEntered = false;
    } else if (message.message_type == c.xdndDrop) {
        if (!m_dndAccepted) {
            finishDrop(false);
            return;
        }
        const auto time = Time(message.data.l[2]);
        XConvertSelection(display, c.xdndSelection, c.uriList, c.dropProperty, m_window, time ? time : CurrentTime);
        XFlush(display);
        // finishDrop runs when the SelectionNotify arrives.
    }
}

void WindowX11::finishDrop(bool converted) {
    X11Connection &c = connection();
    Display *display = c.display;
    if (!m_dndSource) {
        return;
    }
    std::vector<String> paths;
    if (converted) {
        Atom type = 0;
        int format = 0;
        unsigned long items = 0, remaining = 0;
        unsigned char *data = nullptr;
        if (XGetWindowProperty(display, m_window, c.dropProperty, 0, 0x7fffffff, True, AnyPropertyType, &type, &format,
                               &items, &remaining, &data) == Success &&
            data) {
            if (format == 8) {
                paths = pathsFromUriList(StringView(reinterpret_cast<const char *>(data), items));
            }
            XFree(data);
        }
    }
    bool taken = false;
    if (!paths.empty()) {
        DropEvent drop;
        drop.type = DropEvent::Type::Drop;
        drop.position = m_dndPosition;
        drop.paths = std::move(paths);
        taken = handleDrop(drop);
    } else if (m_dndEntered) {
        DropEvent leave;
        leave.type = DropEvent::Type::Leave;
        leave.position = m_dndPosition;
        handleDrop(leave);
    }
    sendXdnd(m_dndSource, c.xdndFinished, taken ? 1 : 0, taken ? long(c.xdndActionCopy) : 0, 0, 0);
    m_dndSource = 0;
    m_dndEntered = false;
    m_dndAccepted = false;
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
    // Files can be dropped on every window (XDND 5); a window without a drop
    // handler refuses them.
    const unsigned long xdndVersion = 5;
    XChangeProperty(display, window, c.xdndAware, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(&xdndVersion), 1);
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

namespace {

::Window selectionWindow(X11Connection &c) {
    if (!c.selectionWindow) {
        XSetWindowAttributes attributes{};
        attributes.event_mask = PropertyChangeMask;
        c.selectionWindow = XCreateWindow(c.display, DefaultRootWindow(c.display), -10, -10, 1, 1, 0, 0,
                                          InputOnly, CopyFromParent, CWEventMask, &attributes);
    }
    return c.selectionWindow;
}

// The largest property one request can carry, in bytes.
std::size_t maxPropertyBytes(Display *display) {
    long units = XExtendedMaxRequestSize(display);
    if (units <= 0) {
        units = XMaxRequestSize(display);
    }
    return std::size_t(std::max(4096L, units * 4 - 1024));
}

// Another client asks for what this process copied.
void answerSelectionRequest(X11Connection &c, const XSelectionRequestEvent &request) {
    XSelectionEvent reply{};
    reply.type = SelectionNotify;
    reply.display = request.display;
    reply.requestor = request.requestor;
    reply.selection = request.selection;
    reply.target = request.target;
    reply.time = request.time;
    // Obsolete clients leave the property unset: the target names it.
    const Atom property = request.property ? request.property : request.target;
    reply.property = 0;
    if (request.selection == c.clipboardAtom && c.ownsClipboard) {
        if (request.target == c.targets) {
            const Atom supported[] = {c.targets, c.utf8String, XA_STRING, c.textAtom};
            XChangeProperty(c.display, request.requestor, property, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char *>(supported), 4);
            reply.property = property;
        } else if ((request.target == c.utf8String || request.target == XA_STRING || request.target == c.textAtom) &&
                   c.clipboard.size() <= maxPropertyBytes(c.display)) {
            const Atom type = request.target == XA_STRING ? XA_STRING : c.utf8String;
            XChangeProperty(c.display, request.requestor, property, type, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char *>(c.clipboard.data()), int(c.clipboard.size()));
            reply.property = property;
        }
    }
    XSendEvent(c.display, request.requestor, False, 0, reinterpret_cast<XEvent *>(&reply));
    XFlush(c.display);
}

// Handles the selection window's events; false if `event` is not one.
bool handleSelectionEvent(X11Connection &c, XEvent &event) {
    if (!c.selectionWindow || event.xany.window != c.selectionWindow) {
        return false;
    }
    if (event.type == SelectionRequest) {
        answerSelectionRequest(c, event.xselectionrequest);
    } else if (event.type == SelectionClear && event.xselectionclear.selection == c.clipboardAtom) {
        c.ownsClipboard = false; // someone else copied
        c.clipboard.clear();
    }
    return true;
}

// Waits (at most until `deadline`) for an event on the selection window
// matching `accept`, answering requests to this process meanwhile.
template <class Accept>
bool waitForSelectionEvent(X11Connection &c, std::chrono::steady_clock::time_point deadline, XEvent &out,
                           Accept accept) {
    while (std::chrono::steady_clock::now() < deadline) {
        while (XPending(c.display)) {
            XEvent event;
            XPeekEvent(c.display, &event);
            if (event.xany.window == c.selectionWindow) {
                XNextEvent(c.display, &event);
                if (accept(event)) {
                    out = event;
                    return true;
                }
                handleSelectionEvent(c, event);
            } else {
                // Leave other windows' events for processEvents: look further.
                bool found = false;
                if (XCheckWindowEvent(c.display, c.selectionWindow, PropertyChangeMask, &event)) {
                    if (accept(event)) {
                        out = event;
                        return true;
                    }
                    found = true;
                }
                if (XCheckTypedWindowEvent(c.display, c.selectionWindow, SelectionNotify, &event)) {
                    if (accept(event)) {
                        out = event;
                        return true;
                    }
                    found = true;
                }
                if (XCheckTypedWindowEvent(c.display, c.selectionWindow, SelectionRequest, &event)) {
                    handleSelectionEvent(c, event);
                    found = true;
                }
                if (!found) {
                    break;
                }
            }
        }
        pollfd fd{ConnectionNumber(c.display), POLLIN, 0};
        ::poll(&fd, 1, 10);
        XEventsQueued(c.display, QueuedAfterReading);
    }
    return false;
}

// Reads and deletes the transfer property; appends its bytes to `out`.
// Returns the property's type (0 if there was none).
Atom takeProperty(X11Connection &c, String &out, std::size_t *itemsRead = nullptr) {
    Atom type = 0;
    int format = 0;
    unsigned long items = 0;
    unsigned long remaining = 0;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(c.display, c.selectionWindow, c.transferProperty, 0, 0x7fffffff, True, AnyPropertyType,
                           &type, &format, &items, &remaining, &data) != Success) {
        return 0;
    }
    if (data) {
        if (format == 8 && type != c.incr) {
            out.append(reinterpret_cast<const char *>(data), items);
        }
        XFree(data);
    }
    if (itemsRead) {
        *itemsRead = items;
    }
    return type;
}

// Latin-1 to UTF-8 (the STRING target).
String latin1ToUtf8(const String &latin1) {
    String out;
    for (const char ch : latin1) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x80) {
            out += ch;
        } else {
            out += char(0xC0 | (byte >> 6));
            out += char(0x80 | (byte & 0x3F));
        }
    }
    return out;
}

std::optional<String> convertSelection(X11Connection &c, Atom target) {
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(1500);
    XDeleteProperty(c.display, c.selectionWindow, c.transferProperty);
    XConvertSelection(c.display, c.clipboardAtom, target, c.transferProperty, c.selectionWindow, CurrentTime);
    XFlush(c.display);
    XEvent event;
    if (!waitForSelectionEvent(c, deadline, event, [&](const XEvent &e) {
            return e.type == SelectionNotify && e.xselection.selection == c.clipboardAtom;
        })) {
        return std::nullopt;
    }
    if (event.xselection.property == 0) {
        return std::nullopt; // the owner cannot give this target
    }
    String text;
    const Atom type = takeProperty(c, text);
    if (type == c.incr) {
        // Incremental: the owner writes chunks, each after we delete the last,
        // and ends with an empty one.
        XFlush(c.display);
        for (;;) {
            if (!waitForSelectionEvent(c, steady_clock::now() + milliseconds(1500), event, [&](const XEvent &e) {
                    return e.type == PropertyNotify && e.xproperty.atom == c.transferProperty &&
                           e.xproperty.state == PropertyNewValue;
                })) {
                return std::nullopt;
            }
            std::size_t items = 0;
            takeProperty(c, text, &items);
            XFlush(c.display);
            if (items == 0) {
                break;
            }
        }
    } else if (type == 0) {
        return std::nullopt;
    }
    if (target == XA_STRING) {
        return latin1ToUtf8(text);
    }
    return text;
}

} // namespace

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
        if (handleSelectionEvent(c, event)) {
            continue;
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

String clipboardText() {
    X11Connection &c = connection();
    if (!c.display) {
        // No display: this process is the whole clipboard.
        return c.ownsClipboard ? c.clipboard : String();
    }
    // Ask the server: another application may have copied since, and its
    // SelectionClear may still be waiting in the queue.
    const ::Window owner = XGetSelectionOwner(c.display, c.clipboardAtom);
    if (c.ownsClipboard && owner == c.selectionWindow) {
        return c.clipboard;
    }
    c.ownsClipboard = false;
    if (owner == 0) {
        return {};
    }
    selectionWindow(c);
    if (std::optional<String> text = convertSelection(c, c.utf8String)) {
        return *text;
    }
    return convertSelection(c, XA_STRING).value_or(String());
}

void setClipboardText(StringView text) {
    X11Connection &c = connection();
    c.clipboard = String(text);
    if (!c.display && !connect()) {
        c.ownsClipboard = true; // no display: this process is the whole clipboard
        return;
    }
    const ::Window owner = selectionWindow(c);
    XSetSelectionOwner(c.display, c.clipboardAtom, owner, CurrentTime);
    c.ownsClipboard = XGetSelectionOwner(c.display, c.clipboardAtom) == owner;
    XFlush(c.display);
}

} // namespace cfw
