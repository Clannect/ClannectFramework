#pragma once

// What the installer tells Windows, all per user (HKEY_CURRENT_USER, no
// administrator rights):
//
//  - an entry in Settings > Apps > Installed apps, whose Uninstall button
//    runs the maintenance tool;
//  - CMake's user package registry, so find_package(ClannectFramework)
//    finds an installed version with no CMAKE_PREFIX_PATH;
//  - the maintenance tool's update check at logon (the Run key).
//
// With CFW_INSTALLER_REGISTRY_SANDBOX set (tests), every key goes under that
// key instead. Elsewhere these do nothing: the installer is for Windows, and its logic is
// tested on every platform.

#include <optional>

#include "cfw/core/Result.h"
#include "cfw/core/String.h"
#include "cfw/io/Path.h"

namespace cfw::installer {

struct UninstallEntry {
    String key;              // "ClannectFramework-0.1.2"
    String displayName;      // "Clannect Framework 0.1.2"
    String displayVersion;   // "0.1.2"
    String publisher;
    Path installLocation;
    String uninstallCommand; // the maintenance tool, quoted, with --uninstall
    String helpLink;
    std::uint32_t estimatedSizeKb = 0;
};

[[nodiscard]] Result<void> registerUninstallEntry(const UninstallEntry &entry);
[[nodiscard]] Result<void> removeUninstallEntry(StringView key);

// Under HKCU\Software\Kitware\CMake\Packages\ClannectFramework\<key>.
[[nodiscard]] Result<void> registerCMakePackage(StringView key, const Path &configFolder);
[[nodiscard]] Result<void> removeCMakePackage(StringView key);

// The command run at logon to check for new versions; nothing removes it.
[[nodiscard]] Result<void> setLogonCommand(const std::optional<String> &command);

// A string value under HKEY_CURRENT_USER (tests read back what was written).
[[nodiscard]] std::optional<String> readUserRegistryString(StringView subkey, StringView name);

inline constexpr const char *kUninstallKey = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";
inline constexpr const char *kCMakePackagesKey = "Software\\Kitware\\CMake\\Packages\\ClannectFramework";
inline constexpr const char *kRunKey = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr const char *kRunValue = "ClannectFrameworkUpdates";

} // namespace cfw::installer
