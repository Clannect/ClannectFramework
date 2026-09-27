// String utilities: trim/split/join, ASCII case folding, and strict,
// locale-independent number parsing and formatting.

#include "cfw/core/Strings.h"

#include <cmath>
#include <limits>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

void trimsAsciiWhitespace() {
    checkEqual(trim("  \t hello \r\n"), StringView("hello"), "trim both ends");
    checkEqual(trimStart("  x "), StringView("x "), "trimStart");
    checkEqual(trimEnd("  x "), StringView("  x"), "trimEnd");
    checkEqual(trim("   "), StringView(""), "all whitespace trims to empty");
}

void splitsAndJoins() {
    const auto parts = split("a,,b,", ',');
    checkEqual(parts.size(), std::size_t(4), "KeepEmpty keeps empty fields");
    checkEqual(parts[1], StringView(""), "empty middle field");
    checkEqual(split("a,,b,", ',', SplitMode::SkipEmpty).size(), std::size_t(2), "SkipEmpty drops them");
    checkEqual(split("", ',').size(), std::size_t(1), "splitting empty text gives one empty field");

    const auto words = split("one::two::three", "::");
    checkEqual(words.size(), std::size_t(3), "multi-character separator");
    checkEqual(join(words, " / "), String("one / two / three"), "join views");
    checkEqual(join(std::vector<String>{"x"}, ","), String("x"), "join one part");
    checkEqual(join(std::vector<String>{}, ","), String(""), "join nothing");
}

void comparesIgnoringAsciiCase() {
    check(equalsIgnoreCase("Workspace", "WORKSPACE"), "equal ignoring case");
    check(!equalsIgnoreCase("Workspace", "Workspaces"), "different lengths");
    check(compareIgnoreCase("apple", "Banana") < 0, "orders ignoring case");
    check(compareIgnoreCase("b", "A") > 0, "orders ignoring case (reverse)");
    check(startsWithIgnoreCase("HTTPS://x", "https://"), "prefix ignoring case");
    checkEqual(toLowerAscii("MiXeD \xC3\x89"), String("mixed \xC3\x89"), "non-ASCII bytes untouched");
}

void parsesIntegersStrictly() {
    checkEqual(parseInt("42").value(), std::int64_t(42), "plain integer");
    checkEqual(parseInt("-9223372036854775808").value(), std::numeric_limits<std::int64_t>::min(), "int64 min");
    check(!parseInt("").ok(), "empty is not a number");
    check(!parseInt(" 1").ok(), "leading whitespace rejected");
    check(!parseInt("1 ").ok(), "trailing text rejected");
    check(!parseInt("+1").ok(), "plus sign rejected");
    check(!parseInt("1.5").ok(), "fraction rejected");
    const auto overflow = parseInt("9223372036854775808");
    check(!overflow.ok() && overflow.error().code() == ErrorCode::OutOfRange, "overflow is OutOfRange");
}

void parsesDoublesStrictly() {
    checkEqual(parseDouble("1.5").value(), 1.5, "decimal");
    checkEqual(parseDouble("-2e3").value(), -2000.0, "exponent");
    check(!parseDouble("1,5").ok(), "comma is never a decimal separator");
    check(!parseDouble("inf").ok(), "infinity rejected");
    check(!parseDouble("nan").ok(), "NaN rejected");
    check(!parseDouble("1e999").ok(), "overflow rejected");
}

void formatsLocaleIndependently() {
    checkEqual(formatInt(-12345), String("-12345"), "integer");
    checkEqual(formatDouble(0.1), String("0.1"), "shortest round-trip text");
    checkEqual(formatDouble(1e21), String("1e+21"), "large values use an exponent");
    const double tricky = 0.30000000000000004;
    checkEqual(parseDouble(formatDouble(tricky)).value(), tricky, "format/parse round-trips exactly");
}

} // namespace

int main() {
    trimsAsciiWhitespace();
    splitsAndJoins();
    comparesIgnoringAsciiCase();
    parsesIntegersStrictly();
    parsesDoublesStrictly();
    formatsLocaleIndependently();
    return cfw::test::finish("StringsTest");
}
