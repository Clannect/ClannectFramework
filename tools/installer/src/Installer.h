#pragma once

// The Clannect Framework installer's logic, without its window: versions,
// the GitHub releases list, the package inside an offline installer, and
// installing and uninstalling a version. main.cpp and Wizard.cpp put a face
// on it; tests/InstallerTest.cpp drives it directly.
//
// Layout on disk (the root defaults to %LOCALAPPDATA%\Programs\ClannectFramework):
//
//     <root>/0.1.2/                  one folder per version: the package's
//     <root>/0.1.2/installed.json    contents, and what the installer knows
//     <root>/maintenance/            the maintenance tool (this program,
//     <root>/maintenance/settings.json   without a package) and its settings
//
// Only folders with installed.json are ever deleted, so an install folder a
// person points at their Documents cannot lose anything but CFW.

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"
#include "cfw/io/Path.h"
#include "cfw/io/Zip.h"

namespace cfw::installer {

inline constexpr const char *kReleasesUrl = "https://api.github.com/repos/Clannect/ClannectFramework/releases";
inline constexpr const char *kProjectUrl = "https://github.com/Clannect/ClannectFramework";
inline constexpr const char *kWindowsPackageSuffix = "-windows-x64-mingw.zip";
inline constexpr const char *kMaintenanceFolder = "maintenance";
#ifdef _WIN32
inline constexpr const char *kMaintenanceTool = "Clannect Framework Maintenance.exe";
#else
inline constexpr const char *kMaintenanceTool = "clannect-framework-maintenance";
#endif

// "0.1.2", "v0.2.0-rc.1": major.minor.patch and an optional pre-release
// suffix, ordered as SemVer orders them (a suffix sorts before the release).
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    String suffix; // without the '-'

    [[nodiscard]] static std::optional<Version> parse(StringView text);
    [[nodiscard]] String toString() const;
    [[nodiscard]] bool isPrerelease() const noexcept { return !suffix.empty(); }
    friend bool operator==(const Version &, const Version &) = default;
};
[[nodiscard]] int compare(const Version &a, const Version &b);
inline bool operator<(const Version &a, const Version &b) { return compare(a, b) < 0; }

struct Asset {
    String name;
    String url;   // browser_download_url
    std::uint64_t size = 0;
    String sha256; // lowercase hex from GitHub's digest, or empty
};

struct Release {
    String tag;
    Version version;
    String title;
    bool prerelease = false;
    String publishedAt; // ISO 8601
    String pageUrl;
    std::optional<Asset> windowsPackage;
};

// GitHub's /releases JSON: published releases (no drafts) whose tag is a
// version, newest version first.
[[nodiscard]] Result<std::vector<Release>> parseReleases(StringView json);

// The package inside an offline installer: a release zip appended to the
// executable. Its one top folder is "ClannectFramework-<version>-windows-x64-mingw".
struct Package {
    Version version;
    ZipArchive archive;
    std::size_t prefixSize = 0; // bytes before the zip: the installer itself
};
// Nothing when `self` carries no zip (the online installer); an error when
// it carries one that is not a Clannect Framework package.
[[nodiscard]] Result<std::optional<Package>> findPackage(Span<const std::byte> self);
// The same checks for a downloaded package.
[[nodiscard]] Result<Package> openPackage(Span<const std::byte> zip);

struct Installed {
    Version version;
    String tag;
    bool prerelease = false;
    Path folder;
};
[[nodiscard]] Path versionFolder(const Path &root, const Version &version);
[[nodiscard]] Path maintenanceTool(const Path &root);
// Versions installed under `root` (folders with installed.json), newest first.
[[nodiscard]] std::vector<Installed> installedVersions(const Path &root);

struct Preferences {
    bool notifyUpdates = true;
    bool includePrereleases = true;
    std::int64_t lastUpdateCheck = 0; // seconds since 1970
    String skippedTag;
};
[[nodiscard]] Preferences loadPreferences(const Path &root);
[[nodiscard]] Result<void> savePreferences(const Path &root, const Preferences &preferences);

struct InstallRequest {
    Path root;
    const Package *package = nullptr;
    String tag;                 // "v0.1.2"
    bool prerelease = false;    // GitHub marks it a pre-release (all of 0.x)
    // This program without a package, copied in as the maintenance tool
    // (empty: leave the tool alone).
    Span<const std::byte> maintenanceTool;
    bool registerWithCMake = true;
    // Online installs: whether the maintenance tool checks for new versions
    // at logon. Offline installs leave it as it was.
    std::optional<bool> notifyUpdates;
    bool includePrereleases = true;
    // Fraction done and what is happening; return false to cancel.
    std::function<bool(double, StringView)> progress;
};
// Installs into versionFolder(root, version): extracts, then registers with
// Windows (Installed apps) and CMake. Replaces an earlier install of the
// same version; refuses a non-empty folder that is not one.
[[nodiscard]] Result<Path> install(const InstallRequest &request);

// Removes one installed version (and, with the last one, the maintenance
// tool and its logon check). `folder` must hold installed.json.
[[nodiscard]] Result<void> uninstall(const Path &folder);

// The newest release worth telling about: it has a Windows package, is
// newer than everything installed, is not a pre-release unless those are
// wanted, and was not skipped.
[[nodiscard]] std::optional<Release> newerRelease(const std::vector<Release> &releases,
                                                  const std::vector<Installed> &installed, const Preferences &prefs);

// The command line Windows runs for a given action, quoted.
[[nodiscard]] String quoteArgument(StringView argument);

} // namespace cfw::installer
