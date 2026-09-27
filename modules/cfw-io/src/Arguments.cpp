#include "cfw/io/Arguments.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include "cfw/core/Utf8.h"
#endif

namespace cfw {

std::vector<String> processArguments(int argc, const char *const *argv) {
    std::vector<String> out;
#ifdef _WIN32
    int count = 0;
    if (LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 0; i < count; ++i) {
            out.push_back(utf16ToUtf8Lossy(std::u16string_view(reinterpret_cast<const char16_t *>(wide[i]))));
        }
        LocalFree(wide);
        return out;
    }
#endif
    for (int i = 0; i < argc; ++i) {
        out.emplace_back(argv[i] ? argv[i] : "");
    }
    return out;
}

} // namespace cfw
