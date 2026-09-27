#include "cfw/core/Utf8.h"

namespace cfw {

namespace {

bool isContinuation(unsigned char byte) noexcept { return (byte & 0xC0u) == 0x80u; }

bool isSurrogate(char32_t cp) noexcept { return cp >= 0xD800u && cp <= 0xDFFFu; }

// The number of bytes that make up the maximal ill-formed subpart starting at
// `offset` (Unicode's "U+FFFD substitution of maximal subparts"): the lead byte
// plus any continuation bytes that could still have been part of a valid
// sequence.
std::uint8_t invalidLength(StringView text, std::size_t offset, std::size_t validPrefix) noexcept {
    const std::size_t n = validPrefix == 0 ? 1 : validPrefix;
    return static_cast<std::uint8_t>(offset + n <= text.size() ? n : text.size() - offset);
}

} // namespace

Utf8Char decodeUtf8At(StringView text, std::size_t offset) noexcept {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[offset + i]); };
    const std::size_t available = text.size() - offset;
    const unsigned char lead = byte(0);

    if (lead < 0x80u) {
        return {lead, 1, true};
    }

    std::size_t length = 0;
    char32_t cp = 0;
    unsigned char lowerBound = 0x80u; // allowed range for the second byte
    unsigned char upperBound = 0xBFu;

    if (lead >= 0xC2u && lead <= 0xDFu) {
        length = 2;
        cp = lead & 0x1Fu;
    } else if (lead >= 0xE0u && lead <= 0xEFu) {
        length = 3;
        cp = lead & 0x0Fu;
        if (lead == 0xE0u) {
            lowerBound = 0xA0u; // no overlongs
        } else if (lead == 0xEDu) {
            upperBound = 0x9Fu; // no surrogates
        }
    } else if (lead >= 0xF0u && lead <= 0xF4u) {
        length = 4;
        cp = lead & 0x07u;
        if (lead == 0xF0u) {
            lowerBound = 0x90u; // no overlongs
        } else if (lead == 0xF4u) {
            upperBound = 0x8Fu; // nothing above U+10FFFF
        }
    } else {
        return {kReplacementCharacter, 1, false}; // C0, C1, F5..FF, or a stray continuation
    }

    for (std::size_t i = 1; i < length; ++i) {
        if (i >= available) {
            return {kReplacementCharacter, invalidLength(text, offset, i), false};
        }
        const unsigned char b = byte(i);
        const bool inRange = i == 1 ? (b >= lowerBound && b <= upperBound) : isContinuation(b);
        if (!inRange) {
            return {kReplacementCharacter, invalidLength(text, offset, i), false};
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    return {cp, static_cast<std::uint8_t>(length), true};
}

std::size_t findInvalidUtf8(StringView text) noexcept {
    std::size_t offset = 0;
    while (offset < text.size()) {
        // Fast path for ASCII runs.
        if (static_cast<unsigned char>(text[offset]) < 0x80u) {
            ++offset;
            continue;
        }
        const Utf8Char decoded = decodeUtf8At(text, offset);
        if (!decoded.valid) {
            return offset;
        }
        offset += decoded.length;
    }
    return text.size();
}

bool isValidUtf8(StringView text) noexcept { return findInvalidUtf8(text) == text.size(); }

void appendUtf8(String &out, char32_t cp) {
    if (isSurrogate(cp) || cp > 0x10FFFFu) {
        cp = kReplacementCharacter;
    }
    if (cp < 0x80u) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800u) {
        out += static_cast<char>(0xC0u | (cp >> 6));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        out += static_cast<char>(0xE0u | (cp >> 12));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (cp >> 18));
        out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    }
}

int compareUtf16Order(StringView a, StringView b) noexcept {
    // UTF-8 byte order is code point order. UTF-16 order differs only in one
    // case: a character above U+FFFF (a surrogate pair, D800-DBFF first) sorts
    // before one in U+E000..U+FFFF. In UTF-8 those start with a 4-byte lead
    // (F0-F4) and with EE or EF. Such bytes differ at the start of a character
    // in both strings, so one byte test at the first difference decides it.
    const std::size_t common = std::min(a.size(), b.size());
    std::size_t k = 0;
    while (k < common && a[k] == b[k]) {
        ++k;
    }
    if (k == common) {
        return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
    }
    const auto x = static_cast<unsigned char>(a[k]);
    const auto y = static_cast<unsigned char>(b[k]);
    const auto supplementary = [](unsigned char lead) { return lead >= 0xF0u; };
    const auto highBmp = [](unsigned char lead) { return lead == 0xEEu || lead == 0xEFu; };
    if (supplementary(x) && highBmp(y)) {
        return -1;
    }
    if (highBmp(x) && supplementary(y)) {
        return 1;
    }
    return x < y ? -1 : 1;
}

std::size_t countCodepoints(StringView text) noexcept {
    std::size_t count = 0;
    for (std::size_t offset = 0; offset < text.size(); ++count) {
        offset += decodeUtf8At(text, offset).length;
    }
    return count;
}

Result<std::u16string> utf8ToUtf16(StringView text) {
    std::u16string out;
    out.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();) {
        const Utf8Char decoded = decodeUtf8At(text, offset);
        if (!decoded.valid) {
            return Error(ErrorCode::ParseError, "invalid UTF-8").with("offset", std::to_string(offset));
        }
        if (decoded.codepoint >= 0x10000u) {
            const char32_t v = decoded.codepoint - 0x10000u;
            out += static_cast<char16_t>(0xD800u + (v >> 10));
            out += static_cast<char16_t>(0xDC00u + (v & 0x3FFu));
        } else {
            out += static_cast<char16_t>(decoded.codepoint);
        }
        offset += decoded.length;
    }
    return out;
}

namespace {

// Decodes UTF-16 into UTF-8. Returns the unit offset of the first unpaired
// surrogate if `strict`, otherwise substitutes U+FFFD and returns text.size().
std::size_t convertUtf16(std::u16string_view text, String &out, bool strict) {
    out.reserve(out.size() + text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char32_t unit = text[i];
        if (unit >= 0xD800u && unit <= 0xDBFFu && i + 1 < text.size() && text[i + 1] >= 0xDC00u &&
            text[i + 1] <= 0xDFFFu) {
            appendUtf8(out, 0x10000u + ((unit - 0xD800u) << 10) + (static_cast<char32_t>(text[i + 1]) - 0xDC00u));
            ++i;
        } else if (isSurrogate(unit)) {
            if (strict) {
                return i;
            }
            appendUtf8(out, kReplacementCharacter);
        } else {
            appendUtf8(out, unit);
        }
    }
    return text.size();
}

} // namespace

Result<String> utf16ToUtf8(std::u16string_view text) {
    String out;
    const std::size_t bad = convertUtf16(text, out, true);
    if (bad != text.size()) {
        return Error(ErrorCode::ParseError, "unpaired UTF-16 surrogate").with("offset", std::to_string(bad));
    }
    return out;
}

String utf16ToUtf8Lossy(std::u16string_view text) {
    String out;
    (void)convertUtf16(text, out, false);
    return out;
}

String sanitizeUtf8(StringView text) {
    String out;
    out.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();) {
        const Utf8Char decoded = decodeUtf8At(text, offset);
        if (decoded.valid) {
            out.append(text.substr(offset, decoded.length));
        } else {
            appendUtf8(out, kReplacementCharacter);
        }
        offset += decoded.length;
    }
    return out;
}

} // namespace cfw
