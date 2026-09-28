// Win32 backend: a window class with CS_DBLCLKS, per-monitor DPI (v2 where
// the OS has it), SetDIBitsToDevice presentation, UTF-16 WM_CHAR text with
// surrogate pairs, mouse capture while a button is held.

#include "PixelCopy.h"
#include "cfw/image/Image.h"
#include "cfw/platform/Window.h"

namespace cfw::win32 {
constexpr Modifier kNoModifier = Modifier::None;
} // namespace cfw::win32

#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#include "cfw/core/Utf8.h"

namespace cfw {

namespace {

constexpr wchar_t kClassName[] = L"CfwWindow";
std::atomic<DWORD> gThread{0};

std::wstring wide(StringView utf8) {
    auto converted = utf8ToUtf16(utf8);
    const std::u16string text = converted ? std::move(converted).value() : std::u16string();
    return std::wstring(text.begin(), text.end());
}

String narrow(const wchar_t *text, std::size_t length) {
    return utf16ToUtf8Lossy(std::u16string_view(reinterpret_cast<const char16_t *>(text), length));
}

UINT dpiOf(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI *)(HWND);
    static const auto fn = reinterpret_cast<GetDpiForWindowFn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (fn && hwnd) {
        const UINT dpi = fn(hwnd);
        if (dpi > 0) {
            return dpi;
        }
    }
    HDC dc = GetDC(nullptr);
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? UINT(dpi) : 96u;
}

void enableDpiAwareness() {
    using SetContextFn = BOOL(WINAPI *)(HANDLE);
    const auto fn = reinterpret_cast<SetContextFn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")));
    if (fn) {
        fn(reinterpret_cast<HANDLE>(-4)); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    } else {
        SetProcessDPIAware();
    }
}

Key keyOf(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return static_cast<Key>(static_cast<unsigned>(Key::A) + (vk - 'A'));
    }
    if (vk >= '0' && vk <= '9') {
        return static_cast<Key>(static_cast<unsigned>(Key::Digit0) + (vk - '0'));
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        return static_cast<Key>(static_cast<unsigned>(Key::F1) + (vk - VK_F1));
    }
    switch (vk) {
    case VK_TAB: return Key::Tab;
    case VK_RETURN: return Key::Enter;
    case VK_ESCAPE: return Key::Escape;
    case VK_SPACE: return Key::Space;
    case VK_BACK: return Key::Backspace;
    case VK_DELETE: return Key::Delete;
    case VK_LEFT: return Key::Left;
    case VK_RIGHT: return Key::Right;
    case VK_UP: return Key::Up;
    case VK_DOWN: return Key::Down;
    case VK_HOME: return Key::Home;
    case VK_END: return Key::End;
    case VK_PRIOR: return Key::PageUp;
    case VK_NEXT: return Key::PageDown;
    case VK_OEM_3: return Key::Backquote;
    case VK_OEM_MINUS: return Key::Minus;
    case VK_OEM_PLUS: return Key::Equal;
    case VK_OEM_4: return Key::BracketLeft;
    case VK_OEM_6: return Key::BracketRight;
    case VK_OEM_5: return Key::Backslash;
    case VK_OEM_1: return Key::Semicolon;
    case VK_OEM_7: return Key::Quote;
    case VK_OEM_COMMA: return Key::Comma;
    case VK_OEM_PERIOD: return Key::Period;
    case VK_OEM_2: return Key::Slash;
    case VK_INSERT: return Key::Insert;
    case VK_SHIFT: return Key::Shift;
    case VK_CONTROL: return Key::Control;
    case VK_MENU: return Key::Alt;
    case VK_LWIN: case VK_RWIN: return Key::Meta;
    default: return Key::Unknown;
    }
}

Modifier currentModifiers() {
    Modifier m = win32::kNoModifier;
    if (GetKeyState(VK_SHIFT) < 0) m = m | Modifier::Shift;
    if (GetKeyState(VK_CONTROL) < 0) m = m | Modifier::Control;
    if (GetKeyState(VK_MENU) < 0) m = m | Modifier::Alt;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) m = m | Modifier::Meta;
    return m;
}

LPCWSTR cursorId(Cursor cursor) {
    switch (cursor) {
    case Cursor::IBeam: return IDC_IBEAM;
    case Cursor::Hand: return IDC_HAND;
    case Cursor::Wait: return IDC_WAIT;
    case Cursor::Crosshair: return IDC_CROSS;
    case Cursor::SizeHorizontal: return IDC_SIZEWE;
    case Cursor::SizeVertical: return IDC_SIZENS;
    case Cursor::SizeDiagonal: return IDC_SIZENWSE;
    case Cursor::SizeAntiDiagonal: return IDC_SIZENESW;
    case Cursor::SizeAll: return IDC_SIZEALL;
    case Cursor::NotAllowed: return IDC_NO;
    default: return IDC_ARROW;
    }
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

void registerClass() {
    static bool registered = false;
    if (registered) {
        return;
    }
    enableDpiAwareness();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS | CS_OWNDC;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
}

class WindowWin32 final : public Window {
public:
    explicit WindowWin32(HWND hwnd) : m_hwnd(hwnd) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        m_scale = float(dpiOf(hwnd)) / 96.0f;
        RECT r{};
        GetClientRect(hwnd, &r);
        m_size = {int(r.right - r.left), int(r.bottom - r.top)};
    }

    ~WindowWin32() override {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd);
    }

    void show() override { ShowWindow(m_hwnd, SW_SHOW); }
    void hide() override { ShowWindow(m_hwnd, SW_HIDE); }
    void setTitle(StringView title) override { SetWindowTextW(m_hwnd, wide(title).c_str()); }

    void setSize(Vec2i size) override {
        RECT r{0, 0, LONG(std::lround(float(size.x) * m_scale)), LONG(std::lround(float(size.y) * m_scale))};
        AdjustWindowRectEx(&r, DWORD(GetWindowLongPtrW(m_hwnd, GWL_STYLE)), FALSE,
                           DWORD(GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE)));
        SetWindowPos(m_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    Vec2i pixelSize() const override { return m_size; }
    float devicePixelRatio() const override { return m_scale; }

    void present(const Image &image) override {
        detail::toBgrx(image, m_frame);
        m_frameSize = {int(image.width()), int(image.height())};
        HDC dc = GetDC(m_hwnd);
        blit(dc);
        ReleaseDC(m_hwnd, dc);
    }

    Result<Image> capture() const override {
        HDC windowDc = GetDC(m_hwnd);
        HDC memory = CreateCompatibleDC(windowDc);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = m_size.x;
        info.bmiHeader.biHeight = -m_size.y;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(windowDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        Result<Image> result = Error(ErrorCode::IoError, "could not capture the window");
        if (bitmap && bits) {
            HGDIOBJ old = SelectObject(memory, bitmap);
            BitBlt(memory, 0, 0, m_size.x, m_size.y, windowDc, 0, 0, SRCCOPY);
            GdiFlush();
            auto created = Image::create(std::uint32_t(m_size.x), std::uint32_t(m_size.y));
            if (created) {
                Image image = std::move(created).value();
                const auto *src = static_cast<const std::uint32_t *>(bits);
                for (int y = 0; y < m_size.y; ++y) {
                    std::uint8_t *row = image.row(std::uint32_t(y)).data();
                    for (int x = 0; x < m_size.x; ++x) {
                        const std::uint32_t p = src[std::size_t(y) * std::size_t(m_size.x) + std::size_t(x)];
                        row[x * 4] = std::uint8_t(p >> 16);
                        row[x * 4 + 1] = std::uint8_t(p >> 8);
                        row[x * 4 + 2] = std::uint8_t(p);
                        row[x * 4 + 3] = 255;
                    }
                }
                result = std::move(image);
            }
            SelectObject(memory, old);
        }
        if (bitmap) {
            DeleteObject(bitmap);
        }
        DeleteDC(memory);
        ReleaseDC(m_hwnd, windowDc);
        return result;
    }

    void requestRepaint() override { InvalidateRect(m_hwnd, nullptr, FALSE); }

    void setCursor(Cursor cursor) override {
        m_cursor = cursor;
        SetCursor(cursor == Cursor::Hidden ? nullptr : LoadCursorW(nullptr, cursorId(cursor)));
    }

    void *nativeHandle() const override { return m_hwnd; }

    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);

private:
    void blit(HDC dc) {
        if (m_frame.empty()) {
            return;
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = m_frameSize.x;
        info.bmiHeader.biHeight = -m_frameSize.y; // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, 0, 0, DWORD(m_frameSize.x), DWORD(m_frameSize.y), 0, 0, 0, UINT(m_frameSize.y),
                          m_frame.data(), &info, DIB_RGB_COLORS);
    }

    Vec2 logical(LPARAM lParam) const {
        return {float(GET_X_LPARAM(lParam)) / m_scale, float(GET_Y_LPARAM(lParam)) / m_scale};
    }

    void mouse(PointerEvent::Type type, PointerButton button, LPARAM lParam, int clicks = 1) {
        PointerEvent e;
        e.type = type;
        e.button = button;
        e.position = logical(lParam);
        e.modifiers = currentModifiers();
        e.clickCount = clicks;
        if (type == PointerEvent::Type::Press) {
            SetCapture(m_hwnd);
            ++m_buttonsDown;
        } else if (type == PointerEvent::Type::Release && m_buttonsDown > 0 && --m_buttonsDown == 0) {
            ReleaseCapture();
        }
        pointer.emit(e);
    }

    HWND m_hwnd;
    float m_scale = 1.0f;
    Vec2i m_size;
    std::vector<std::uint32_t> m_frame;
    Vec2i m_frameSize;
    Cursor m_cursor = Cursor::Arrow;
    bool m_tracking = false;
    int m_buttonsDown = 0;
    wchar_t m_highSurrogate = 0;
};

LRESULT WindowWin32::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE: {
        const Vec2i size{int(LOWORD(lParam)), int(HIWORD(lParam))};
        if (!(size == m_size) && wParam != SIZE_MINIMIZED) {
            m_size = size;
            resized.emit(size);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(m_hwnd, &ps);
        blit(dc);
        EndPaint(m_hwnd, &ps);
        repaintRequested.emit();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        closeRequested.emit();
        return 0;
    case WM_SETFOCUS:
        focusChanged.emit(true);
        return 0;
    case WM_KILLFOCUS:
        focusChanged.emit(false);
        return 0;
    case WM_DPICHANGED: {
        m_scale = float(HIWORD(wParam)) / 96.0f;
        const RECT *suggested = reinterpret_cast<const RECT *>(lParam);
        SetWindowPos(m_hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        dpiChanged.emit(m_scale);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(m_cursor == Cursor::Hidden ? nullptr : LoadCursorW(nullptr, cursorId(m_cursor)));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        if (!m_tracking) {
            TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, m_hwnd, 0};
            TrackMouseEvent(&track);
            m_tracking = true;
        }
        mouse(PointerEvent::Type::Move, PointerButton::None, lParam);
        return 0;
    case WM_MOUSELEAVE:
        m_tracking = false;
        mouse(PointerEvent::Type::Leave, PointerButton::None, 0);
        return 0;
    case WM_LBUTTONDOWN: mouse(PointerEvent::Type::Press, PointerButton::Left, lParam); return 0;
    case WM_LBUTTONDBLCLK: mouse(PointerEvent::Type::Press, PointerButton::Left, lParam, 2); return 0;
    case WM_LBUTTONUP: mouse(PointerEvent::Type::Release, PointerButton::Left, lParam); return 0;
    case WM_RBUTTONDOWN: mouse(PointerEvent::Type::Press, PointerButton::Right, lParam); return 0;
    case WM_RBUTTONDBLCLK: mouse(PointerEvent::Type::Press, PointerButton::Right, lParam, 2); return 0;
    case WM_RBUTTONUP: mouse(PointerEvent::Type::Release, PointerButton::Right, lParam); return 0;
    case WM_MBUTTONDOWN: mouse(PointerEvent::Type::Press, PointerButton::Middle, lParam); return 0;
    case WM_MBUTTONDBLCLK: mouse(PointerEvent::Type::Press, PointerButton::Middle, lParam, 2); return 0;
    case WM_MBUTTONUP: mouse(PointerEvent::Type::Release, PointerButton::Middle, lParam); return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(m_hwnd, &p); // wheel positions are in screen coordinates
        PointerEvent e;
        e.type = PointerEvent::Type::Wheel;
        e.position = {float(p.x) / m_scale, float(p.y) / m_scale};
        e.modifiers = currentModifiers();
        const float amount = float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA) * 48.0f; // 3 lines of 16 px
        e.wheelDelta = message == WM_MOUSEWHEEL ? Vec2{0, amount} : Vec2{-amount, 0};
        pointer.emit(e);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        KeyEvent e;
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        e.type = down ? KeyEvent::Type::Press : KeyEvent::Type::Release;
        e.key = keyOf(wParam);
        e.modifiers = currentModifiers();
        e.repeat = down && (lParam & (1 << 30)) != 0;
        key.emit(e);
        if (message == WM_SYSKEYDOWN && wParam == VK_F4) {
            break; // Alt+F4 still closes
        }
        return (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) ? DefWindowProcW(m_hwnd, message, wParam, lParam) : 0;
    }
    case WM_CHAR: {
        const wchar_t unit = wchar_t(wParam);
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            m_highSurrogate = unit;
            return 0;
        }
        wchar_t units[2] = {unit, 0};
        std::size_t count = 1;
        if (m_highSurrogate && unit >= 0xDC00 && unit <= 0xDFFF) {
            units[0] = m_highSurrogate;
            units[1] = unit;
            count = 2;
        }
        m_highSurrogate = 0;
        if (units[0] >= 0x20 && units[0] != 0x7F) { // not Ctrl+letter, Backspace, Enter...
            text.emit(TextEvent{narrow(units, count)});
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto *window = reinterpret_cast<WindowWin32 *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (window) {
        return window->handle(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

Result<std::unique_ptr<Window>> Window::create(const WindowOptions &options) {
    registerClass();
    gThread = GetCurrentThreadId();
    const DWORD style = options.resizable ? WS_OVERLAPPEDWINDOW : (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX);
    HWND hwnd = CreateWindowExW(0, kClassName, wide(options.title).c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                options.size.x, options.size.y, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        return Error(ErrorCode::Unsupported, "CreateWindowExW failed");
    }
    auto window = std::make_unique<WindowWin32>(hwnd);
    window->setSize(options.size); // the client area, at the window's DPI
    if (options.visible) {
        window->show(); // the first WM_PAINT arrives through processEvents()
    }
    return std::unique_ptr<Window>(std::move(window));
}

bool processEvents(Duration maxWait) {
    MSG msg;
    if (!PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE)) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(maxWait).count();
        MsgWaitForMultipleObjectsEx(0, nullptr, DWORD(std::clamp<long long>(ms, 0, 0x7FFFFFFF)), QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
    }
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

void wakeUp() {
    const DWORD thread = gThread;
    if (thread) {
        PostThreadMessageW(thread, WM_NULL, 0, 0);
    }
}

String clipboardText() {
    String out;
    if (!OpenClipboard(nullptr)) {
        return out;
    }
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto *text = static_cast<const wchar_t *>(GlobalLock(data))) {
            out = narrow(text, wcslen(text));
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return out;
}

void setClipboardText(StringView utf8) {
    const std::wstring text = wide(utf8);
    if (!OpenClipboard(nullptr)) {
        return;
    }
    EmptyClipboard();
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
        if (void *target = GlobalLock(memory)) {
            std::memcpy(target, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) {
                GlobalFree(memory);
            }
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

} // namespace cfw
