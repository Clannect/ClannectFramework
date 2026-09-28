#include "cfw/io/Environment.h"

#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <vector>

#include "cfw/core/Utf8.h"
#endif

namespace cfw {

std::optional<String> environmentVariable(StringView name) {
#ifdef _WIN32
    const Result<std::u16string> wideName = utf8ToUtf16(name);
    if (!wideName) {
        return std::nullopt;
    }
    const auto *key = reinterpret_cast<const wchar_t *>(wideName.value().c_str());
    SetLastError(ERROR_SUCCESS);
    const DWORD needed = GetEnvironmentVariableW(key, nullptr, 0);
    if (needed == 0) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            return std::nullopt;
        }
        return String(); // set, but empty
    }
    std::vector<wchar_t> buffer(needed);
    const DWORD length = GetEnvironmentVariableW(key, buffer.data(), needed);
    return utf16ToUtf8Lossy(std::u16string_view(reinterpret_cast<const char16_t *>(buffer.data()), length));
#else
    const String key(name);
    if (const char *value = std::getenv(key.c_str())) {
        return String(value);
    }
    return std::nullopt;
#endif
}

} // namespace cfw
