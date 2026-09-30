#include "System.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <string>

#include "cfw/core/Utf8.h"
#endif

namespace cfw::installer {

#ifdef _WIN32

namespace {

// Tests move every key under a key of their own, so running them on a
// developer's machine never touches that person's real entries.
String sandboxed(StringView subkey) {
    const char *prefix = std::getenv("CFW_INSTALLER_REGISTRY_SANDBOX");
    return prefix && *prefix ? String(prefix) + "\\" + String(subkey) : String(subkey);
}

std::wstring wide(StringView text) {
    const Result<std::u16string> converted = utf8ToUtf16(text);
    return converted ? std::wstring(converted.value().begin(), converted.value().end()) : std::wstring();
}

String narrow(const std::wstring &text) {
    const std::u16string u16(text.begin(), text.end());
    return utf16ToUtf8(u16).valueOr({});
}

String windowsPath(const Path &path) {
    String text = path.toString();
    std::replace(text.begin(), text.end(), '/', '\\');
    return text;
}

Error failure(StringView what, LSTATUS status) {
    return Error(ErrorCode::IoError, String(what) + " (Windows error " + std::to_string(status) + ")");
}

// An open key under HKEY_CURRENT_USER, created if needed.
class Key {
public:
    explicit Key(StringView subkey) {
        m_status = RegCreateKeyExW(HKEY_CURRENT_USER, wide(sandboxed(subkey)).c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                   KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &m_key, nullptr);
    }
    ~Key() {
        if (m_status == ERROR_SUCCESS) {
            RegCloseKey(m_key);
        }
    }
    Key(const Key &) = delete;
    Key &operator=(const Key &) = delete;

    [[nodiscard]] LSTATUS status() const { return m_status; }
    LSTATUS setString(StringView name, StringView value) {
        const std::wstring data = wide(value);
        return RegSetValueExW(m_key, wide(name).c_str(), 0, REG_SZ, reinterpret_cast<const BYTE *>(data.c_str()),
                              static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    }
    LSTATUS setNumber(StringView name, std::uint32_t value) {
        const DWORD data = value;
        return RegSetValueExW(m_key, wide(name).c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&data),
                              sizeof data);
    }
    LSTATUS remove(StringView name) { return RegDeleteValueW(m_key, wide(name).c_str()); }

private:
    HKEY m_key = nullptr;
    LSTATUS m_status = ERROR_INVALID_HANDLE;
};

} // namespace

Result<void> registerUninstallEntry(const UninstallEntry &entry) {
    Key key(String(kUninstallKey) + entry.key);
    if (key.status() != ERROR_SUCCESS) {
        return failure("could not create the Installed apps entry", key.status());
    }
    LSTATUS status = ERROR_SUCCESS;
    for (const auto &[name, value] : {std::pair<const char *, String>{"DisplayName", entry.displayName},
                                      {"DisplayVersion", entry.displayVersion},
                                      {"Publisher", entry.publisher},
                                      {"InstallLocation", windowsPath(entry.installLocation)},
                                      {"UninstallString", entry.uninstallCommand},
                                      {"HelpLink", entry.helpLink},
                                      {"URLInfoAbout", entry.helpLink}}) {
        if (status == ERROR_SUCCESS) {
            status = key.setString(name, value);
        }
    }
    for (const auto &[name, value] : {std::pair<const char *, std::uint32_t>{"NoModify", 1},
                                      {"NoRepair", 1},
                                      {"EstimatedSize", entry.estimatedSizeKb}}) {
        if (status == ERROR_SUCCESS) {
            status = key.setNumber(name, value);
        }
    }
    return status == ERROR_SUCCESS ? Result<void>() : Result<void>(failure("could not write the Installed apps entry", status));
}

Result<void> removeUninstallEntry(StringView key) {
    const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, wide(sandboxed(String(kUninstallKey) + String(key))).c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND
               ? Result<void>()
               : Result<void>(failure("could not remove the Installed apps entry", status));
}

Result<void> registerCMakePackage(StringView name, const Path &configFolder) {
    // CMake reads every value of the package's key as a folder holding the
    // package's config file (cmake-packages(7), "User Package Registry").
    Key key(kCMakePackagesKey);
    if (key.status() != ERROR_SUCCESS) {
        return failure("could not create the CMake package registry entry", key.status());
    }
    const LSTATUS status = key.setString(name, windowsPath(configFolder));
    return status == ERROR_SUCCESS ? Result<void>() : Result<void>(failure("could not register with CMake", status));
}

Result<void> removeCMakePackage(StringView name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, wide(sandboxed(kCMakePackagesKey)).c_str(), 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return {};
    }
    RegDeleteValueW(key, wide(name).c_str());
    DWORD values = 0;
    RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &values, nullptr, nullptr, nullptr,
                     nullptr);
    RegCloseKey(key);
    if (values == 0) {
        RegDeleteKeyW(HKEY_CURRENT_USER, wide(sandboxed(kCMakePackagesKey)).c_str());
    }
    return {};
}

Result<void> setLogonCommand(const std::optional<String> &command) {
    Key key(kRunKey);
    if (key.status() != ERROR_SUCCESS) {
        return failure("could not open the logon programs", key.status());
    }
    if (!command) {
        const LSTATUS status = key.remove(kRunValue);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND
                   ? Result<void>()
                   : Result<void>(failure("could not remove the update check", status));
    }
    const LSTATUS status = key.setString(kRunValue, *command);
    return status == ERROR_SUCCESS ? Result<void>() : Result<void>(failure("could not add the update check", status));
}

std::optional<String> readUserRegistryString(StringView subkey, StringView name) {
    wchar_t buffer[4096];
    DWORD size = sizeof buffer;
    if (RegGetValueW(HKEY_CURRENT_USER, wide(sandboxed(subkey)).c_str(), wide(name).c_str(), RRF_RT_REG_SZ, nullptr, buffer,
                     &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return narrow(std::wstring(buffer));
}

#else

Result<void> registerUninstallEntry(const UninstallEntry &) { return {}; }
Result<void> removeUninstallEntry(StringView) { return {}; }
Result<void> registerCMakePackage(StringView, const Path &) { return {}; }
Result<void> removeCMakePackage(StringView) { return {}; }
Result<void> setLogonCommand(const std::optional<String> &) { return {}; }
std::optional<String> readUserRegistryString(StringView, StringView) { return std::nullopt; }

#endif

} // namespace cfw::installer
