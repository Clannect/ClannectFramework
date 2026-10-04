// What WindowTest does to a window through Win32 itself (Windows only): posts
// WM_DROPFILES as the shell does, composes text in the window's input
// method context as an IME does, posts key messages with the scan codes a
// keyboard sends, and moves the mouse as a device does.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <imm.h>
#include <shlobj.h>

#include <cstring>
#include <string>
#include <vector>

bool postFileDrop(void *hwnd, const std::vector<std::u16string> &paths, int x, int y) {
    std::u16string list;
    for (const std::u16string &path : paths) {
        list += path;
        list += u'\0';
    }
    list += u'\0';
    HGLOBAL memory = GlobalAlloc(GHND, sizeof(DROPFILES) + list.size() * sizeof(char16_t));
    if (!memory) {
        return false;
    }
    auto *files = static_cast<DROPFILES *>(GlobalLock(memory));
    files->pFiles = sizeof(DROPFILES);
    files->pt = POINT{x, y};
    files->fWide = TRUE;
    std::memcpy(reinterpret_cast<char *>(files) + sizeof(DROPFILES), list.data(), list.size() * sizeof(char16_t));
    GlobalUnlock(memory);
    // The window frees the block (DragFinish) when it handles the message.
    return PostMessageW(static_cast<HWND>(hwnd), WM_DROPFILES, reinterpret_cast<WPARAM>(memory), 0) != 0;
}

// A key message as the keyboard driver posts it: the virtual key the layout
// gives the key, and in lParam the key's scan code and extended flag.
bool postKey(void *hwnd, unsigned virtualKey, unsigned scanCode, bool extended, bool down) {
    LPARAM lParam = 1 | LPARAM(scanCode) << 16 | (extended ? LPARAM(1) << 24 : 0);
    if (!down) {
        lParam |= LPARAM(3) << 30;
    }
    return PostMessageW(static_cast<HWND>(hwnd), down ? WM_KEYDOWN : WM_KEYUP, virtualKey, lParam) != 0;
}

// Makes the window the focused foreground window, if the system lets a
// background process do that.
bool focusWindow(void *hwnd) {
    const auto window = static_cast<HWND>(hwnd);
    SetForegroundWindow(window);
    SetFocus(window);
    return GetFocus() == window && GetForegroundWindow() == window;
}

// Moves the mouse by (dx, dy) counts, as a mouse does (through the input
// stack, so Raw Input sees it).
bool moveMouseBy(int dx, int dy) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    return SendInput(1, &input, sizeof input) == 1;
}

// The rectangle the cursor is clipped to, and whether it is the window's
// client area (in screen coordinates).
bool cursorClippedToClient(void *hwnd) {
    const auto window = static_cast<HWND>(hwnd);
    RECT clip{}, client{};
    GetClipCursor(&clip);
    GetClientRect(window, &client);
    MapWindowPoints(window, nullptr, reinterpret_cast<POINT *>(&client), 2);
    return EqualRect(&clip, &client) != 0;
}

// Tells the window it lost the keyboard focus, as Windows does on Alt+Tab.
void sendFocusLost(void *hwnd) { SendMessageW(static_cast<HWND>(hwnd), WM_KILLFOCUS, 0, 0); }
void sendFocusGained(void *hwnd) { SendMessageW(static_cast<HWND>(hwnd), WM_SETFOCUS, 0, 0); }

// Sets the composition string on the window's input context, as an IME
// does while the user types; `commit` then completes it (the IME's Enter).
// Returns false if the window has no input context.
bool imeCompose(void *hwnd, std::u16string text, bool commit) {
    const auto window = static_cast<HWND>(hwnd);
    HIMC context = ImmGetContext(window);
    if (!context) {
        return false;
    }
    ImmSetOpenStatus(context, TRUE);
    const BOOL set = ImmSetCompositionStringW(context, SCS_SETSTR, static_cast<void *>(text.data()), DWORD(text.size() * sizeof(char16_t)),
                                              nullptr, 0);
    if (commit) {
        ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
    }
    ImmReleaseContext(window, context);
    return set != FALSE;
}
