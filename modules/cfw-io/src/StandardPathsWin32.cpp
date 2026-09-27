// Windows implementation of StandardPaths.h (Known Folders).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include "cfw/io/StandardPaths.h"

namespace cfw {

namespace {

Result<Path> knownFolder(REFKNOWNFOLDERID id, const char *name) {
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(hr) || raw == nullptr) {
        if (raw != nullptr) {
            CoTaskMemFree(raw);
        }
        return Error(ErrorCode::NotFound, "known folder unavailable")
            .with("folder", name)
            .with("hresult", std::to_string(static_cast<long>(hr)));
    }
    Path path{std::filesystem::path(raw)};
    CoTaskMemFree(raw);
    return path;
}

Result<Path> appFolder(REFKNOWNFOLDERID id, const char *name, const AppIdentity &app) {
    Result<Path> base = knownFolder(id, name);
    if (!base) {
        return base;
    }
    return base.value() / app.organization / app.application;
}

} // namespace

Result<Path> appDataDirectory(const AppIdentity &app) { return appFolder(FOLDERID_RoamingAppData, "RoamingAppData", app); }

Result<Path> localAppDataDirectory(const AppIdentity &app) {
    return appFolder(FOLDERID_LocalAppData, "LocalAppData", app);
}

Result<Path> cacheDirectory(const AppIdentity &app) {
    Result<Path> local = localAppDataDirectory(app);
    if (!local) {
        return local;
    }
    return local.value() / "cache";
}

Result<Path> documentsDirectory() { return knownFolder(FOLDERID_Documents, "Documents"); }

Result<Path> homeDirectory() { return knownFolder(FOLDERID_Profile, "Profile"); }

} // namespace cfw
