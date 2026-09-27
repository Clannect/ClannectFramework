#pragma once

// Per-user standard directories. The layout matches what the Qt build used
// (QStandardPaths with the organisation and application names), so data,
// caches and settings written before the port are found after it:
//
//                 Windows                              Linux                         macOS
//   appData       %APPDATA%\<org>\<app>                ~/.local/share/<org>/<app>    ~/Library/Application Support/<org>/<app>
//   localAppData  %LOCALAPPDATA%\<org>\<app>           ~/.local/share/<org>/<app>    ~/Library/Application Support/<org>/<app>
//   cache         %LOCALAPPDATA%\<org>\<app>\cache     ~/.cache/<org>/<app>          ~/Library/Caches/<org>/<app>
//   documents     the user's Documents folder
//
// XDG_DATA_HOME / XDG_CACHE_HOME are honoured on Linux. Directories are
// returned, not created: call createDirectories() before writing.
//
// Threads: any. Allocates: the returned path.

#include "cfw/core/Result.h"
#include "cfw/core/String.h"
#include "cfw/io/Path.h"

namespace cfw {

struct AppIdentity {
    String organization; // "Clannect"
    String application;  // "Clannect Engine"
};

[[nodiscard]] Result<Path> appDataDirectory(const AppIdentity &app);
[[nodiscard]] Result<Path> localAppDataDirectory(const AppIdentity &app);
[[nodiscard]] Result<Path> cacheDirectory(const AppIdentity &app);
[[nodiscard]] Result<Path> documentsDirectory();
[[nodiscard]] Result<Path> homeDirectory();

} // namespace cfw
