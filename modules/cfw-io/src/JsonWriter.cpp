#include "cfw/io/JsonWriter.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <system_error>

#include "cfw/core/Contract.h"

namespace cfw {

namespace {

constexpr double kMaxExactInteger = 9007199254740992.0; // 2^53

void appendInteger(String &out, std::int64_t value) {
    std::array<char, 24> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    require(ec == std::errc(), "integer formatting buffer too small");
    out.append(buffer.data(), ptr);
}

void appendString(String &out, StringView text) {
    constexpr char kHex[] = "0123456789abcdef";
    out += '"';
    for (char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20u) {
                out += "\\u00";
                out += kHex[(c >> 4) & 0x0F];
                out += kHex[c & 0x0F];
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void appendIndent(String &out, int depth) { out.append(static_cast<std::size_t>(depth) * 4, ' '); }

void appendValue(String &out, const JsonValue &value, JsonFormat format, int depth) {
    const bool indented = format == JsonFormat::Indented;
    switch (value.type()) {
    case JsonType::Null: out += "null"; break;
    case JsonType::Bool: out += *value.asBool() ? "true" : "false"; break;
    case JsonType::Integer: appendInteger(out, *value.asInteger()); break;
    case JsonType::Double: appendJsonNumber(out, *value.asDouble()); break;
    case JsonType::String: appendString(out, *value.asString()); break;
    case JsonType::Array: {
        const JsonArray &array = *value.asArray();
        out += '[';
        for (std::size_t i = 0; i < array.size(); ++i) {
            if (indented) {
                out += '\n';
                appendIndent(out, depth + 1);
            }
            appendValue(out, array[i], format, depth + 1);
            if (i + 1 < array.size()) {
                out += ',';
            }
        }
        if (indented) {
            out += '\n';
            appendIndent(out, depth);
        }
        out += ']';
        break;
    }
    case JsonType::Object: {
        const JsonObject &object = *value.asObject();
        out += '{';
        std::size_t i = 0;
        for (const auto &[key, member] : object) {
            if (indented) {
                out += '\n';
                appendIndent(out, depth + 1);
            }
            appendString(out, key);
            out += indented ? ": " : ":";
            appendValue(out, member, format, depth + 1);
            if (++i < object.size()) {
                out += ',';
            }
        }
        if (indented) {
            out += '\n';
            appendIndent(out, depth);
        }
        out += '}';
        break;
    }
    }
}

} // namespace

void appendJsonNumber(String &out, double value) {
    if (!std::isfinite(value)) {
        out += "null";
        return;
    }
    if (std::trunc(value) == value && std::abs(value) <= kMaxExactInteger) {
        appendInteger(out, static_cast<std::int64_t>(value)); // also turns -0 into 0
        return;
    }

    // Shortest round-trip digits, in scientific form: "-d.ddde[+-]XX".
    std::array<char, 40> buffer{};
    const auto [end, ec] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::scientific);
    require(ec == std::errc(), "double formatting buffer too small");
    const StringView sci(buffer.data(), static_cast<std::size_t>(end - buffer.data()));

    const bool negative = sci.front() == '-';
    const std::size_t ePos = sci.find('e');
    String digits;
    for (char c : sci.substr(negative ? 1 : 0, ePos - (negative ? 1 : 0))) {
        if (c != '.') {
            digits += c;
        }
    }
    int exponent = 0;
    (void)std::from_chars(sci.data() + ePos + 1 + (sci[ePos + 1] == '+' ? 1 : 0), sci.data() + sci.size(), exponent);

    // Plain form.
    String plain;
    const int digitCount = static_cast<int>(digits.size());
    if (exponent >= 0) {
        if (exponent + 1 >= digitCount) {
            plain = digits + String(static_cast<std::size_t>(exponent + 1 - digitCount), '0');
        } else {
            plain = digits.substr(0, static_cast<std::size_t>(exponent + 1)) + "." +
                    digits.substr(static_cast<std::size_t>(exponent + 1));
        }
    } else {
        plain = "0." + String(static_cast<std::size_t>(-exponent - 1), '0') + digits;
    }

    // Exponent form, with at least two exponent digits.
    String scientific = digits.substr(0, 1);
    if (digits.size() > 1) {
        scientific += '.';
        scientific += digits.substr(1);
    }
    scientific += exponent < 0 ? "e-" : "e+";
    const int magnitude = exponent < 0 ? -exponent : exponent;
    if (magnitude < 10) {
        scientific += '0';
    }
    scientific += std::to_string(magnitude);

    if (negative) {
        out += '-';
    }
    out += scientific.size() < plain.size() ? scientific : plain;
}

String writeJson(const JsonValue &value, JsonFormat format) {
    String out;
    appendValue(out, value, format, 0);
    if (format == JsonFormat::Indented) {
        out += '\n';
    }
    return out;
}

} // namespace cfw
