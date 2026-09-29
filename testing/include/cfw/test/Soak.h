#pragma once

// Soak tests: long, randomised runs looking for what short tests miss
// (leaks, slow drift, rare interleavings). They run briefly by default so
// every build runs them, and for as long as asked with
// CFW_SOAK_SECONDS=<seconds>; CFW_SOAK_SEED=<n> replays a run (the seed of
// every run is printed).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

#if defined(__linux__)
#include <dirent.h>
#endif

namespace cfw::test {

inline std::chrono::duration<double> soakDuration(double defaultSeconds) {
    if (const char *text = std::getenv("CFW_SOAK_SECONDS"); text && *text) {
        return std::chrono::duration<double>(std::atof(text));
    }
    return std::chrono::duration<double>(defaultSeconds);
}

inline std::uint64_t soakSeed(const char *suite) {
    std::uint64_t seed = 0;
    if (const char *text = std::getenv("CFW_SOAK_SEED"); text && *text) {
        seed = std::strtoull(text, nullptr, 10);
    } else {
        seed = std::random_device{}();
    }
    std::printf("%s: seed %llu (CFW_SOAK_SEED=%llu replays it)\n", suite, static_cast<unsigned long long>(seed),
                static_cast<unsigned long long>(seed));
    return seed;
}

// Open file descriptors of this process (Linux; -1 elsewhere).
inline int openFileDescriptors() {
#if defined(__linux__)
    int count = 0;
    if (DIR *dir = opendir("/proc/self/fd")) {
        while (readdir(dir)) {
            ++count;
        }
        closedir(dir);
    }
    return count;
#else
    return -1;
#endif
}

} // namespace cfw::test
