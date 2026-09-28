// What WindowTest does to a window through Win32 itself (Windows only): posts
// WM_DROPFILES as the shell does, and composes text in the window's input
// method context as an IME does.

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
