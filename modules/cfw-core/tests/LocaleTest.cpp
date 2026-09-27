// Locale: CLDR separators for common locales, formatting with grouping and
// rounding, and parsing what people type.

#include "cfw/core/Locale.h"

#include <cstdint>
#include <limits>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

void resolvesNames() {
    checkEqual(Locale::fromName("de_DE.UTF-8").name(), StringView("de-DE"), "POSIX names are normalised");
    checkEqual(Locale::fromName("de-DE").decimalSeparator(), StringView(","), "de-DE falls back to de");
    checkEqual(Locale::fromName("de-ch").groupSeparator(), StringView("’"), "de-CH has its own data");
    checkEqual(Locale::fromName("pt-BR").decimalSeparator(), StringView(","), "pt-BR");
    checkEqual(Locale::fromName("xx-YY").decimalSeparator(), StringView("."), "unknown names use English conventions");
    check(Locale::fromName("C") == Locale::c() && Locale::fromName("POSIX") == Locale::c(), "C and POSIX");
    checkEqual(Locale::fromName("sv").minusSign(), StringView("−"), "Swedish uses U+2212");
}

void formats() {
    const Locale en = Locale::fromName("en-US");
    const Locale de = Locale::fromName("de");
    const Locale fr = Locale::fromName("fr-FR");
    checkEqual(en.formatInteger(1234567), String("1,234,567"), "en integer");
    checkEqual(de.formatInteger(-1234567), String("-1.234.567"), "de negative integer");
    checkEqual(fr.formatInteger(1234), String("1 234"), "fr uses a narrow no-break space");
    checkEqual(en.formatInteger(999), String("999"), "no separator under 1000");
    checkEqual(en.formatInteger(1234, false), String("1234"), "grouping can be turned off");
    checkEqual(Locale::c().formatInteger(1234567), String("1234567"), "C does not group");
    checkEqual(en.formatInteger(std::numeric_limits<std::int64_t>::min()), String("-9,223,372,036,854,775,808"),
               "INT64_MIN");
    checkEqual(en.formatNumber(1234.5, 2), String("1,234.50"), "en decimals");
    checkEqual(de.formatNumber(1234.5, 2), String("1.234,50"), "de decimals");
    checkEqual(en.formatNumber(2.5, 0), String("3"), "half rounds away from zero");
    checkEqual(en.formatNumber(-2.5, 0), String("-3"), "on both sides");
    checkEqual(en.formatNumber(1.005, 2), String("1.01"), "1.005 shows as 1.01, as people expect");
    checkEqual(en.formatNumber(-0.001, 2), String("0.00"), "never -0.00");
    checkEqual(Locale::fromName("sv").formatNumber(-1.5, 1), String("−1,5"), "the locale's minus sign");
    checkEqual(en.formatNumber(std::numeric_limits<double>::quiet_NaN(), 2), String("NaN"), "NaN");
    checkEqual(en.formatNumber(-std::numeric_limits<double>::infinity(), 2), String("-∞"), "-infinity");
    checkEqual(en.formatNumber(1e20, 1), String("100,000,000,000,000,000,000.0"), "large values stay exact");
}

void parses() {
    const Locale en = Locale::fromName("en");
    const Locale de = Locale::fromName("de");
    const Locale fr = Locale::fromName("fr");
    checkEqual(en.parseNumber("1,234.5").valueOr(0), 1234.5, "en with grouping");
    checkEqual(en.parseNumber("  -12.25 ").valueOr(0), -12.25, "whitespace and minus");
    checkEqual(de.parseNumber("1.234,5").valueOr(0), 1234.5, "de with grouping");
    checkEqual(de.parseNumber("0,75").valueOr(0), 0.75, "de decimal");
    checkEqual(fr.parseNumber("1 234,5").valueOr(0), 1234.5, "fr accepts a typed ASCII space");
    checkEqual(Locale::fromName("sv").parseNumber("−3").valueOr(0), -3.0, "the locale's minus");
    checkEqual(en.parseNumber(",5").valueOr(1), 1.0, "a leading separator is rejected");
    check(!en.parseNumber("1,23.4"), "groups must have three digits");
    check(!en.parseNumber("1234,567"), "a group before a long run is rejected");
    check(!en.parseNumber("1e5"), "no exponents");
    check(!en.parseNumber("abc") && !en.parseNumber("") && !en.parseNumber("-"), "garbage is rejected");
    check(!de.parseNumber("1.5"), "in German, '.' is not a decimal point");
    checkEqual(en.parseNumber(".5").valueOr(0), 0.5, "a leading decimal point is fine");
    // Round trip.
    for (const char *name : {"en", "de", "fr", "de-CH", "sv", "ja"}) {
        const Locale l = Locale::fromName(name);
        const String s = l.formatNumber(-9876543.21, 2);
        check(l.parseNumber(s).valueOr(0) == -9876543.21, "formatted numbers parse back");
    }
}

} // namespace

int main() {
    resolvesNames();
    formats();
    parses();
    return cfw::test::finish("LocaleTest");
}
