#pragma once

// Locale-dependent number formatting: the minimal replacement for the
// QLocale uses in Clannect (showing numbers to people). Scene files, JSON and
// the network never use this; they use the locale-independent functions in
// Strings.h.
//
// The data (decimal separator, grouping separator, minus sign) comes from
// CLDR for about thirty common locales, built in. There is no global or
// "current" locale: callers pass one explicitly, and cfw-platform reports
// the user's locale name (Locale::fromName turns it into one of these).
// Grouping is always in threes; locales with other grouping (en-IN lakh
// grouping) fall back to their language's default.
//
// Threads: a value type. Allocates: the formatted strings.

#include <cstdint>

#include "cfw/core/Result.h"
#include "cfw/core/String.h"

namespace cfw {

class Locale {
public:
    // The "C" locale: '.' decimal separator, no grouping, ASCII minus.
    [[nodiscard]] static Locale c() noexcept;
    // From a BCP 47 or POSIX name: "de", "de-DE", "de_DE.UTF-8", "pt-BR",
    // "de-CH". Matching tries language-region, then language; an unknown
    // name gives English conventions ('.' and ',') under the name given.
    [[nodiscard]] static Locale fromName(StringView name);

    [[nodiscard]] StringView name() const noexcept { return m_name; }
    [[nodiscard]] StringView decimalSeparator() const noexcept { return m_decimal; }
    [[nodiscard]] StringView groupSeparator() const noexcept { return m_group; }
    [[nodiscard]] StringView minusSign() const noexcept { return m_minus; }

    // 1234567 -> "1,234,567" (en), "1.234.567" (de), "1 234 567" (fr).
    [[nodiscard]] String formatInteger(std::int64_t value, bool grouping = true) const;
    // Fixed number of decimals (0-17), rounded half away from zero:
    // formatNumber(1234.5, 2) -> "1,234.50" (en), "1.234,50" (de). NaN and
    // infinities give "NaN" and "∞" (with the minus sign).
    [[nodiscard]] String formatNumber(double value, int decimals, bool grouping = true) const;

    // Parses what a person typed in this locale: an optional minus (ASCII
    // '-' is accepted too), digits with optional group separators in the
    // right places, and an optional decimal part. Surrounding whitespace is
    // ignored. Fails with ParseError; never accepts exponents or non-finite
    // values.
    [[nodiscard]] Result<double> parseNumber(StringView text) const;

    friend bool operator==(const Locale &a, const Locale &b) = default;

private:
    Locale(String name, StringView decimal, StringView group, StringView minus)
        : m_name(std::move(name)), m_decimal(decimal), m_group(group), m_minus(minus) {}

    String m_name;
    String m_decimal;
    String m_group;
    String m_minus;
};

} // namespace cfw
