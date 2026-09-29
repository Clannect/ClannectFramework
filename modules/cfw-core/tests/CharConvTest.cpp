// cfw::fromChars: the portable floating-point parser (used where the
// standard library has no floating-point std::from_chars, as on Apple's
// libc++) agrees with std::from_chars on edge cases and on random input:
// the same error, the same stopping point, and the same bits.

#include "cfw/core/CharConv.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;

namespace {

int mismatches = 0;

template <class T>
void compare(const std::string &text) {
    T expected = T(1234.5);
    T actual = T(1234.5);
    const char *first = text.data();
    const char *last = text.data() + text.size();
    const auto want = std::from_chars(first, last, expected, std::chars_format::general);
    const auto got = detail::fromCharsFallback(first, last, actual);
    bool same = want.ec == got.ec;
    if (same && want.ec == std::errc()) {
        same = want.ptr == got.ptr && (std::memcmp(&expected, &actual, sizeof(T)) == 0 ||
                                       (expected != expected && actual != actual)); // NaN payloads may differ
    } else if (same && want.ec == std::errc::result_out_of_range) {
        same = actual == T(1234.5); // untouched
    }
    if (!same) {
        if (++mismatches <= 10) {
            std::printf("mismatch (%s) on \"%s\": std ec=%d ptr=%td value=%.17g, fallback ec=%d ptr=%td value=%.17g\n",
                        sizeof(T) == 8 ? "double" : "float", text.c_str(), int(want.ec), want.ptr - first,
                        double(expected), int(got.ec), got.ptr - first, double(actual));
        }
    }
}

void both(const std::string &text) {
    compare<double>(text);
    compare<float>(text);
}

} // namespace

int main() {
    for (const char *text :
         {"0", "-0", "1", "-1", "1.5", ".5", "5.", ".", "-.", "-", "", "+1", " 1", "1 ", "1e", "1e+", "1e-", "1e5",
          "1E-5", "1e+308", "1e309", "-1e309", "1e-320", "1e-400", "-1e-400", "2.2250738585072011e-308",
          "4.9406564584124654e-324", "2.4703282292062327e-324", "2.4703282292062328e-324", "1.7976931348623157e308",
          "1.7976931348623158e308", "1.7976931348623159e308", "3.4028235e38", "3.4028236e38", "1e39", "1e-46",
          "1.401298464e-45", "0x1p3", "0x", "00012", "1.2.3", "1e5e5", "inf", "-inf", "INF", "Infinity", "infinit",
          "nan", "-nan", "NaN", "nan(123)", "nan(abc", "nan()", "in", "n", "abc", "e5", "9007199254740993",
          "0.1", "0.30000000000000004", "123456789012345678901234567890", "1e-7", "7.038531e-26",
          "0.000000000000000000000000000000000000000000001", "1..2", "--1", "-+1"}) {
        both(text);
    }
    // A long number (past the fallback's small buffer).
    both(std::string(100, '1') + "." + std::string(300, '9') + "e-100");

    std::mt19937_64 random(12345);
    for (int i = 0; i < 200000; ++i) {
        std::uint64_t bits = random();
        double d = 0;
        std::memcpy(&d, &bits, sizeof d);
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, i % 3 == 0 ? "%.17g" : i % 3 == 1 ? "%.6g" : "%.25e", d);
        both(buffer);
        // Random strings over the characters numbers are made of.
        static const char alphabet[] = "0123456789.eE+-infa ";
        std::string junk;
        const int length = int(random() % 12);
        for (int k = 0; k < length; ++k) {
            junk.push_back(alphabet[random() % (sizeof alphabet - 1)]);
        }
        both(junk);
    }
    check(mismatches == 0, "the fallback agrees with std::from_chars everywhere");

    double value = 0;
    const char text[] = "2.5x";
    const auto r = fromChars(text, text + 4, value);
    check(r.ec == std::errc() && r.ptr == text + 3 && value == 2.5, "cfw::fromChars stops where the number ends");
    return cfw::test::finish("CharConvTest");
}
