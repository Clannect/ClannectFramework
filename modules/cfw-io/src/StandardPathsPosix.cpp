// Linux and macOS implementation of StandardPaths.h.

#include <pwd.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <string>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <cstdlib>

#include "cfw/io/StandardPaths.h"

namespace cfw {

namespace {

Result<Path> home() {
    if (const char *env = std::getenv("HOME"); env != nullptr && env[0] != '\0') {
        return Path(env);
    }
    if (const passwd *entry = ::getpwuid(::getuid()); entry != nullptr && entry->pw_dir != nullptr) {
        return Path(entry->pw_dir);
    }
    return Error(ErrorCode::NotFound, "home directory unknown");
}

// $variable if set to an absolute path, else ~/fallback.
Result<Path> xdg(const char *variable, const char *fallback) {
    if (const char *env = std::getenv(variable); env != nullptr && env[0] == '/') {
        return Path(env);
    }
    Result<Path> base = home();
    if (!base) {
        return base;
    }
    return base.value() / fallback;
}

} // namespace

Result<Path> homeDirectory() { return home(); }

Result<Path> executablePath() {
#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return Error(ErrorCode::NotFound, "executable path unknown");
    }
    buffer.resize(std::strlen(buffer.c_str()));
    return Path(std::filesystem::weakly_canonical(std::filesystem::path(buffer)));
#else
    std::error_code error;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        return Error(ErrorCode::NotFound, "executable path unknown");
    }
    return Path(self);
#endif
}

#if defined(__APPLE__)

Result<Path> appDataDirectory(const AppIdentity &app) {
    Result<Path> base = home();
    if (!base) {
        return base;
    }
    return base.value() / "Library/Application Support" / app.organization / app.application;
}

Result<Path> cacheDirectory(const AppIdentity &app) {
    Result<Path> base = home();
    if (!base) {
        return base;
    }
    return base.value() / "Library/Caches" / app.organization / app.application;
}

#else

Result<Path> appDataDirectory(const AppIdentity &app) {
    Result<Path> base = xdg("XDG_DATA_HOME", ".local/share");
    if (!base) {
        return base;
    }
    return base.value() / app.organization / app.application;
}

Result<Path> cacheDirectory(const AppIdentity &app) {
    Result<Path> base = xdg("XDG_CACHE_HOME", ".cache");
    if (!base) {
        return base;
    }
    return base.value() / app.organization / app.application;
}

#endif

Result<Path> localAppDataDirectory(const AppIdentity &app) { return appDataDirectory(app); }

Result<Path> documentsDirectory() {
    Result<Path> base = home();
    if (!base) {
        return base;
    }
    return base.value() / "Documents";
}

} // namespace cfw
