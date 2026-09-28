// Posts WM_DROPFILES with a DROPFILES block, as the shell does for windows
// that accept files without an OLE drop target (WindowTest, Windows only).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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
