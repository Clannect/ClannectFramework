// Runs a fuzz target over files instead of under libFuzzer: every argument is
// a file or a directory of files. This is how the committed corpus runs as a
// CTest test on every toolchain, MinGW included, so an input that once
// crashed stays fixed. It is also how a crash file from a fuzzing run is
// reproduced in a debugger.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "targets/FuzzTarget.h"

namespace {

bool runFile(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path.string().c_str());
        return false;
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    (void)LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
    return true;
}

} // namespace

int main(int argc, char **argv) {
    int files = 0;
    bool ok = true;
    for (int i = 1; i < argc; ++i) {
        const std::filesystem::path arg(argv[i]);
        if (std::filesystem::is_directory(arg)) {
            for (const auto &entry : std::filesystem::recursive_directory_iterator(arg)) {
                if (entry.is_regular_file()) {
                    ok = runFile(entry.path()) && ok;
                    ++files;
                }
            }
        } else {
            ok = runFile(arg) && ok;
            ++files;
        }
    }
    // An empty corpus means a wrong path, not a pass.
    if (files == 0) {
        std::fprintf(stderr, "no inputs found\n");
        return 1;
    }
    std::printf("OK: %d inputs replayed\n", files);
    return ok ? 0 : 1;
}
