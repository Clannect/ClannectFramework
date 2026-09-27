#pragma once

// Plain assertions in the engine's test style: every failed check prints and
// counts, the test keeps going, and main() returns cfw::test::finish().

#include <cmath>
#include <cstdio>
#include <source_location>
#include <sstream>
#include <string>

namespace cfw::test {

inline int gChecks = 0;
inline int gFailures = 0;

inline void reportFailure(const char *message, const std::string &detail, std::source_location where) {
    std::printf("FAIL: %s%s  [%s:%u]\n", message, detail.c_str(), where.file_name(),
                static_cast<unsigned>(where.line()));
    ++gFailures;
}

inline void check(bool condition, const char *message,
                  std::source_location where = std::source_location::current()) {
    ++gChecks;
    if (!condition) {
        reportFailure(message, std::string(), where);
    }
}

template <class T>
std::string describe(const T &value) {
    if constexpr (requires(std::ostream &out) { out << value; }) {
        std::ostringstream out;
        out << value;
        return out.str();
    } else {
        return "<unprintable>";
    }
}

template <class A, class B>
void checkEqual(const A &actual, const B &expected, const char *message,
                std::source_location where = std::source_location::current()) {
    ++gChecks;
    if (!(actual == expected)) {
        reportFailure(message, " (expected " + describe(expected) + ", got " + describe(actual) + ")", where);
    }
}

inline void checkNear(double actual, double expected, double tolerance, const char *message,
                      std::source_location where = std::source_location::current()) {
    ++gChecks;
    if (!(std::abs(actual - expected) <= tolerance)) {
        reportFailure(message, " (expected " + describe(expected) + ", got " + describe(actual) + ")", where);
    }
}

// Prints the summary and returns the process exit code.
inline int finish(const char *suite) {
    if (gFailures == 0) {
        std::printf("OK: %s (%d checks)\n", suite, gChecks);
        return 0;
    }
    std::printf("%s: %d of %d checks failed\n", suite, gFailures, gChecks);
    return 1;
}

} // namespace cfw::test
