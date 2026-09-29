#include "cfw/core/Strings.h"
#include "cfw/core/CharConv.h"

#include <array>
#include <charconv>
#include <cmath>
#include <system_error>

#include "cfw/core/Contract.h"

namespace cfw {

namespace {

// A short, printable excerpt of untrusted input for error messages.
String excerpt(StringView text) {
    constexpr std::size_t kMax = 32;
    String out(text.substr(0, kMax));
    if (text.size() > kMax) {
        out += "...";
    }
    return out;
}

template <class Parts>
String joinParts(const Parts &parts, StringView separator) {
    std::size_t total = 0;
    for (const auto &part : parts) {
        total += part.size() + separator.size();
    }
    String out;
    out.reserve(total);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out += separator;
        }
        out += parts[i];
    }
    return out;
}

} // namespace

StringView trimStart(StringView text) noexcept {
    std::size_t start = 0;
    while (start < text.size() && isAsciiSpace(text[start])) {
        ++start;
    }
    return text.substr(start);
}

StringView trimEnd(StringView text) noexcept {
    std::size_t end = text.size();
    while (end > 0 && isAsciiSpace(text[end - 1])) {
        --end;
    }
    return text.substr(0, end);
}

StringView trim(StringView text) noexcept { return trimEnd(trimStart(text)); }

std::vector<StringView> split(StringView text, char separator, SplitMode mode) {
    return split(text, StringView(&separator, 1), mode);
}

std::vector<StringView> split(StringView text, StringView separator, SplitMode mode) {
    require(!separator.empty(), "split() separator must not be empty");
    std::vector<StringView> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t found = text.find(separator, start);
        const StringView part = text.substr(start, found == StringView::npos ? StringView::npos : found - start);
        if (mode == SplitMode::KeepEmpty || !part.empty()) {
            parts.push_back(part);
        }
        if (found == StringView::npos) {
            break;
        }
        start = found + separator.size();
    }
    return parts;
}

String join(const std::vector<StringView> &parts, StringView separator) { return joinParts(parts, separator); }
String join(const std::vector<String> &parts, StringView separator) { return joinParts(parts, separator); }

String toLowerAscii(StringView text) {
    String out(text);
    for (char &c : out) {
        c = toLowerAscii(c);
    }
    return out;
}

String toUpperAscii(StringView text) {
    String out(text);
    for (char &c : out) {
        c = toUpperAscii(c);
    }
    return out;
}

int compareIgnoreCase(StringView a, StringView b) noexcept {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto ca = static_cast<unsigned char>(toLowerAscii(a[i]));
        const auto cb = static_cast<unsigned char>(toLowerAscii(b[i]));
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
    }
    if (a.size() == b.size()) {
        return 0;
    }
    return a.size() < b.size() ? -1 : 1;
}

bool equalsIgnoreCase(StringView a, StringView b) noexcept {
    return a.size() == b.size() && compareIgnoreCase(a, b) == 0;
}

bool startsWithIgnoreCase(StringView text, StringView prefix) noexcept {
    return text.size() >= prefix.size() && equalsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

Result<std::int64_t> parseInt(StringView text) {
    std::int64_t value = 0;
    const char *end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec == std::errc::result_out_of_range) {
        return Error(ErrorCode::OutOfRange, "integer out of range").with("text", excerpt(text));
    }
    if (ec != std::errc() || ptr != end || text.empty()) {
        return Error(ErrorCode::ParseError, "not an integer").with("text", excerpt(text));
    }
    return value;
}

Result<double> parseDouble(StringView text) {
    double value = 0.0;
    const char *end = text.data() + text.size();
    const auto [ptr, ec] = fromChars(text.data(), end, value);
    if (ec == std::errc::result_out_of_range) {
        return Error(ErrorCode::OutOfRange, "number out of range").with("text", excerpt(text));
    }
    if (ec != std::errc() || ptr != end || text.empty()) {
        return Error(ErrorCode::ParseError, "not a number").with("text", excerpt(text));
    }
    if (!std::isfinite(value)) {
        return Error(ErrorCode::ParseError, "number is not finite").with("text", excerpt(text));
    }
    return value;
}

String formatInt(std::int64_t value) {
    std::array<char, 24> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    require(ec == std::errc(), "formatInt buffer too small");
    return String(buffer.data(), ptr);
}

String formatDouble(double value) {
    std::array<char, 32> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    require(ec == std::errc(), "formatDouble buffer too small");
    return String(buffer.data(), ptr);
}

} // namespace cfw
