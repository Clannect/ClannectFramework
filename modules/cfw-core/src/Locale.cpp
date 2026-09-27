#include "cfw/core/Locale.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>

#include "cfw/core/Strings.h"

namespace cfw {

namespace {

// CLDR number symbols (latn numbering system). U+00A0 is a no-break space,
// U+202F a narrow no-break space, U+2019 a right single quote, U+2212 minus.
struct LocaleData {
    const char *tag; // "de" or "de-CH"
    const char *decimal;
    const char *group;
    const char *minus;
};

constexpr const char *kNbsp = " ";
constexpr const char *kNarrowNbsp = " ";
constexpr const char *kMinus = "−";

constexpr std::array<LocaleData, 33> kLocales = {{
    {"en", ".", ",", "-"},
    {"de", ",", ".", "-"},
    {"de-CH", ".", "’", "-"},
    {"de-AT", ",", kNbsp, "-"},
    {"fr", ",", kNarrowNbsp, "-"},
    {"fr-CH", ",", kNarrowNbsp, "-"},
    {"es", ",", ".", "-"},
    {"es-MX", ".", ",", "-"},
    {"it", ",", ".", "-"},
    {"it-CH", ".", "’", "-"},
    {"nl", ",", ".", "-"},
    {"pt", ",", ".", "-"},
    {"pt-PT", ",", kNbsp, "-"},
    {"ru", ",", kNbsp, "-"},
    {"uk", ",", kNbsp, "-"},
    {"pl", ",", kNbsp, "-"},
    {"cs", ",", kNbsp, "-"},
    {"sk", ",", kNbsp, "-"},
    {"hu", ",", kNbsp, "-"},
    {"ro", ",", ".", "-"},
    {"sv", ",", kNbsp, kMinus},
    {"nb", ",", kNbsp, kMinus},
    {"no", ",", kNbsp, kMinus},
    {"da", ",", ".", "-"},
    {"fi", ",", kNbsp, kMinus},
    {"tr", ",", ".", "-"},
    {"el", ",", ".", "-"},
    {"id", ",", ".", "-"},
    {"vi", ",", ".", "-"},
    {"ja", ".", ",", "-"},
    {"zh", ".", ",", "-"},
    {"ko", ".", ",", "-"},
    {"th", ".", ",", "-"},
}};

// "de_DE.UTF-8@euro" -> "de-DE"; lower-case language, upper-case region.
String normalise(StringView name) {
    String out;
    for (const char c : name) {
        if (c == '.' || c == '@') {
            break;
        }
        out += c == '_' ? '-' : c;
    }
    const std::size_t dash = out.find('-');
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = i < dash ? toLowerAscii(out[i]) : toUpperAscii(out[i]);
    }
    return out;
}

const LocaleData *find(StringView tag) {
    for (const LocaleData &d : kLocales) {
        if (tag == d.tag) {
            return &d;
        }
    }
    return nullptr;
}

// Inserts the group separator every three digits of `digits`.
String group(StringView digits, StringView separator) {
    String out;
    const std::size_t n = digits.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0 && (n - i) % 3 == 0) {
            out += separator;
        }
        out += digits[i];
    }
    return out;
}

} // namespace

Locale Locale::c() noexcept { return Locale("C", ".", "", "-"); }

Locale Locale::fromName(StringView name) {
    const String tag = normalise(name);
    if (tag == "C" || tag == "c" || tag == "POSIX" || tag == "posix") {
        return c();
    }
    const LocaleData *data = find(tag);
    if (data == nullptr) {
        const std::size_t dash = tag.find('-');
        data = find(dash == String::npos ? StringView(tag) : StringView(tag).substr(0, dash));
    }
    if (data == nullptr) {
        data = &kLocales[0];
    }
    return Locale(tag, data->decimal, data->group, data->minus);
}

String Locale::formatInteger(std::int64_t value, bool grouping) const {
    // The magnitude as unsigned, so INT64_MIN works.
    const std::uint64_t magnitude = value < 0 ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    char buffer[24];
    const auto end = std::to_chars(buffer, buffer + sizeof buffer, magnitude).ptr;
    const StringView digits(buffer, static_cast<std::size_t>(end - buffer));
    String out = value < 0 ? m_minus : String();
    out += grouping && !m_group.empty() ? group(digits, m_group) : String(digits);
    return out;
}

String Locale::formatNumber(double value, int decimals, bool grouping) const {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return (value < 0 ? m_minus : String()) + "∞";
    }
    decimals = decimals < 0 ? 0 : decimals > 17 ? 17 : decimals;
    // Round the shortest decimal form (what a person typed or saw: 1.005, not
    // the binary 1.00499999...), half away from zero.
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, std::abs(value), std::chars_format::scientific);
    const StringView sci(buffer, static_cast<std::size_t>(result.ptr - buffer));
    const std::size_t e = sci.find('e');
    String mantissa;
    for (const char c : sci.substr(0, e)) {
        if (c != '.') {
            mantissa += c;
        }
    }
    int exponent = 0;
    std::from_chars(sci.data() + e + 1 + (sci[e + 1] == '+' ? 1 : 0), sci.data() + sci.size(), exponent);
    // Digits before the decimal point: exponent + 1 (pad with zeros).
    const int intDigits = exponent + 1;
    String digits; // all digits, with `decimals` of them after the point
    const int total = intDigits + decimals;
    for (int i = std::min(0, intDigits); i < std::max(total, 0); ++i) {
        digits += (i >= 0 && i < static_cast<int>(mantissa.size())) ? mantissa[static_cast<std::size_t>(i)] : '0';
    }
    // Leading zeros for |value| < 1 were added above when intDigits <= 0.
    const bool roundUp = total >= 0 && total < static_cast<int>(mantissa.size()) && mantissa[static_cast<std::size_t>(total)] >= '5';
    if (total < 0 || digits.empty()) {
        digits.assign(static_cast<std::size_t>(std::max(decimals, 0) + 1), '0');
    }
    if (roundUp) {
        std::size_t i = digits.size();
        while (i > 0) {
            --i;
            if (digits[i] == '9') {
                digits[i] = '0';
            } else {
                ++digits[i];
                break;
            }
            if (i == 0) {
                digits.insert(digits.begin(), '1');
            }
        }
    }
    const auto dec = static_cast<std::size_t>(decimals);
    if (digits.size() <= dec) {
        digits.insert(0, dec + 1 - digits.size(), '0');
    }
    String whole = digits.substr(0, digits.size() - dec);
    const String fractionDigits = digits.substr(digits.size() - dec);
    const std::size_t firstNonZero = whole.find_first_not_of('0');
    whole = firstNonZero == String::npos ? String("0") : whole.substr(firstNonZero);
    const bool isZero = whole == "0" && fractionDigits.find_first_not_of('0') == String::npos;
    String out = value < 0 && !isZero ? m_minus : String(); // never "-0.00"
    out += grouping && !m_group.empty() ? group(whole, m_group) : String(whole);
    if (decimals > 0) {
        out += m_decimal;
        out += fractionDigits;
    }
    return out;
}

Result<double> Locale::parseNumber(StringView text) const {
    const auto fail = [&] { return Error(ErrorCode::ParseError, "not a number in this locale").with("text", String(text)); };
    text = trim(text);
    bool negative = false;
    if (!m_minus.empty() && text.substr(0, m_minus.size()) == m_minus) {
        negative = true;
        text.remove_prefix(m_minus.size());
    } else if (!text.empty() && text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    // Split off the decimal part.
    StringView whole = text;
    StringView fraction;
    const std::size_t dec = text.find(m_decimal);
    if (dec != StringView::npos) {
        whole = text.substr(0, dec);
        fraction = text.substr(dec + m_decimal.size());
    }
    // Whole part: digits, with group separators only between groups of three.
    // A plain ASCII space is accepted where the separator is a no-break space.
    String digits;
    std::size_t sinceGroup = 0;
    bool sawGroup = false;
    while (!whole.empty()) {
        const char c = whole.front();
        if (c >= '0' && c <= '9') {
            digits += c;
            ++sinceGroup;
            whole.remove_prefix(1);
            continue;
        }
        std::size_t skip = 0;
        if (!m_group.empty() && whole.substr(0, m_group.size()) == m_group) {
            skip = m_group.size();
        } else if (c == ' ' && (m_group == " " || m_group == " ")) {
            skip = 1;
        }
        if (skip == 0 || digits.empty() || (sawGroup && sinceGroup != 3) || (!sawGroup && sinceGroup > 3)) {
            return fail();
        }
        sawGroup = true;
        sinceGroup = 0;
        whole.remove_prefix(skip);
    }
    if (sawGroup && sinceGroup != 3) {
        return fail();
    }
    for (const char c : fraction) {
        if (c < '0' || c > '9') {
            return fail();
        }
    }
    if (digits.empty() && fraction.empty()) {
        return fail();
    }
    String canonical = digits.empty() ? String("0") : digits;
    if (!fraction.empty()) {
        canonical += '.';
        canonical += fraction;
    }
    Result<double> parsed = parseDouble(canonical);
    if (!parsed) {
        return fail();
    }
    return negative ? -parsed.value() : parsed.value();
}

} // namespace cfw
