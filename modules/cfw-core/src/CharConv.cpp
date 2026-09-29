#include "cfw/core/CharConv.h"

#include <cerrno>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace cfw {

namespace {

#if !defined(_WIN32)
bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

// Whether [p, last) starts with `word` (lower case), ignoring case.
bool startsWith(const char *p, const char *last, const char *word) noexcept {
    for (; *word; ++word, ++p) {
        if (p == last || lower(*p) != *word) {
            return false;
        }
    }
    return true;
}

// The length of the longest prefix of [first, last) that from_chars
// (general) would consume: [-] then digits with an optional fraction and
// exponent, or inf / infinity / nan / nan(chars). 0: none.
std::size_t validPrefix(const char *first, const char *last) noexcept {
    const char *p = first;
    if (p != last && *p == '-') {
        ++p;
    }
    if (startsWith(p, last, "infinity")) {
        return std::size_t(p - first) + 8;
    }
    if (startsWith(p, last, "inf")) {
        return std::size_t(p - first) + 3;
    }
    if (startsWith(p, last, "nan")) {
        p += 3;
        if (p != last && *p == '(') {
            const char *q = p + 1;
            while (q != last && (isDigit(*q) || (lower(*q) >= 'a' && lower(*q) <= 'z') || *q == '_')) {
                ++q;
            }
            if (q != last && *q == ')') {
                p = q + 1;
            }
        }
        return std::size_t(p - first);
    }
    bool digits = false;
    while (p != last && isDigit(*p)) {
        ++p;
        digits = true;
    }
    if (p != last && *p == '.') {
        const char *q = p + 1;
        bool fraction = false;
        while (q != last && isDigit(*q)) {
            ++q;
            fraction = true;
        }
        if (digits || fraction) {
            p = q;
            digits = true;
        }
    }
    if (!digits) {
        return 0;
    }
    if (p != last && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1;
        if (q != last && (*q == '+' || *q == '-')) {
            ++q;
        }
        if (q != last && isDigit(*q)) {
            while (q != last && isDigit(*q)) {
                ++q;
            }
            p = q;
        }
    }
    return std::size_t(p - first);
}

using CLocale = locale_t;
CLocale cLocale() {
    static const CLocale locale = newlocale(LC_ALL_MASK, "C", locale_t(nullptr));
    return locale;
}
double toDouble(const char *text, char **end) { return strtod_l(text, end, cLocale()); }
float toFloat(const char *text, char **end) { return strtof_l(text, end, cLocale()); }

template <class T, class Convert>
std::from_chars_result parse(const char *first, const char *last, T &value, Convert convert) noexcept {
    const std::size_t length = validPrefix(first, last);
    if (length == 0) {
        return {first, std::errc::invalid_argument};
    }
    // strtod needs a terminated string; numbers are short.
    char small[64];
    std::string large;
    const char *text = small;
    if (length < sizeof small) {
        std::memcpy(small, first, length);
        small[length] = '\0';
    } else {
        try {
            large.assign(first, length);
        } catch (...) {
            return {first, std::errc::not_enough_memory};
        }
        text = large.c_str();
    }
    char *end = nullptr;
    errno = 0;
    const T result = convert(text, &end);
    const int error = errno;
    const char *ptr = first + (end - text);
    if (error == ERANGE && (std::isinf(result) || result == T(0))) {
        return {ptr, std::errc::result_out_of_range};
    }
    value = result;
    return {ptr, std::errc()};
}
#endif

} // namespace

namespace detail {

#if defined(_WIN32)
// Every Windows toolchain has floating-point std::from_chars (and the
// C runtime MinGW links has no _strtof_l).
std::from_chars_result fromCharsFallback(const char *first, const char *last, double &value) noexcept {
    return std::from_chars(first, last, value, std::chars_format::general);
}
std::from_chars_result fromCharsFallback(const char *first, const char *last, float &value) noexcept {
    return std::from_chars(first, last, value, std::chars_format::general);
}
#else
std::from_chars_result fromCharsFallback(const char *first, const char *last, double &value) noexcept {
    return parse(first, last, value, toDouble);
}
std::from_chars_result fromCharsFallback(const char *first, const char *last, float &value) noexcept {
    return parse(first, last, value, toFloat);
}
#endif

} // namespace detail

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
std::from_chars_result fromChars(const char *first, const char *last, double &value) noexcept {
    return std::from_chars(first, last, value, std::chars_format::general);
}
std::from_chars_result fromChars(const char *first, const char *last, float &value) noexcept {
    return std::from_chars(first, last, value, std::chars_format::general);
}
#else
std::from_chars_result fromChars(const char *first, const char *last, double &value) noexcept {
    return detail::fromCharsFallback(first, last, value);
}
std::from_chars_result fromChars(const char *first, const char *last, float &value) noexcept {
    return detail::fromCharsFallback(first, last, value);
}
#endif

} // namespace cfw
