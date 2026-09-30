#pragma once

// The desktop around an application: the system's file and folder pickers, showing a
// folder in the file manager, and opening a link in the default browser
// (QFileDialog::getOpenFileNames and QDesktopServices).
//
// Windows uses the common file dialog and the shell, macOS NSOpenPanel and
// NSWorkspace. Other desktops use
// what they install for this: zenity (GNOME and most others) or kdialog
// (KDE) for the picker, xdg-open for folders and links. Where none is
// present the calls fail with Unsupported, and the application can ask in
// its own interface instead.
//
// Threads: the window's. The pickers block until they close.

#include <optional>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/String.h"
#include "cfw/io/Path.h"

namespace cfw {

class Window;

struct FileFilter {
    String name;                   // "Images"
    std::vector<String> patterns;  // {"*.png", "*.jpg"}
};

struct OpenFileOptions {
    String title = "Open";
    std::vector<FileFilter> filters; // the first is selected; empty: all files
    bool multiple = false;
    Path directory;                  // where it starts; empty: the system's choice
};

// The files chosen, or none if the picker was cancelled.
[[nodiscard]] Result<std::vector<Path>> chooseFilesToOpen(const Window *owner, const OpenFileOptions &options);

struct ChooseFolderOptions {
    String title = "Choose a folder";
    Path directory; // where it starts; empty: the system's choice
};

// The folder chosen, or nothing if the picker was cancelled. The picker can
// create a new folder.
[[nodiscard]] Result<std::optional<Path>> chooseFolder(const Window *owner, const ChooseFolderOptions &options);

// Shows a folder in the file manager.
[[nodiscard]] Result<void> showInFileManager(const Path &folder);

// Opens an http(s) link in the default browser.
[[nodiscard]] Result<void> openUrl(StringView url);

} // namespace cfw
