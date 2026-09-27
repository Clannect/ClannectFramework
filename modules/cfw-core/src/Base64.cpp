#include "cfw/core/Base64.h"

#include <array>
#include <cstdint>

namespace cfw {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::array<std::int8_t, 256> makeDecodeTable() {
    std::array<std::int8_t, 256> table{};
    for (auto &entry : table) {
        entry = -1;
    }
    for (int i = 0; i < 64; ++i) {
        table[static_cast<unsigned char>(kAlphabet[i])] = static_cast<std::int8_t>(i);
    }
    return table;
}

constexpr std::array<std::int8_t, 256> kDecode = makeDecodeTable();

Error invalid(const char *message, std::size_t offset) {
    return Error(ErrorCode::ParseError, message).with("offset", std::to_string(offset));
}

} // namespace

String base64Encode(Span<const std::byte> data) {
    String out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    const auto at = [&](std::size_t k) { return std::to_integer<std::uint32_t>(data[k]); };
    for (; i + 3 <= data.size(); i += 3) {
        const std::uint32_t v = at(i) << 16 | at(i + 1) << 8 | at(i + 2);
        out += kAlphabet[v >> 18 & 63];
        out += kAlphabet[v >> 12 & 63];
        out += kAlphabet[v >> 6 & 63];
        out += kAlphabet[v & 63];
    }
    const std::size_t rest = data.size() - i;
    if (rest == 1) {
        const std::uint32_t v = at(i) << 16;
        out += kAlphabet[v >> 18 & 63];
        out += kAlphabet[v >> 12 & 63];
        out += "==";
    } else if (rest == 2) {
        const std::uint32_t v = at(i) << 16 | at(i + 1) << 8;
        out += kAlphabet[v >> 18 & 63];
        out += kAlphabet[v >> 12 & 63];
        out += kAlphabet[v >> 6 & 63];
        out += '=';
    }
    return out;
}

String base64Encode(StringView text) {
    return base64Encode(Span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size()));
}

Result<std::vector<std::byte>> base64Decode(StringView text) {
    if (text.size() % 4 != 0) {
        return invalid("base64 length is not a multiple of 4", text.size());
    }
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        int padding = 0;
        std::uint32_t v = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const char c = text[i + k];
            if (c == '=') {
                // Padding only in the last group, only in its last two places,
                // and nothing but padding after it.
                if (!last || k < 2 || (k == 2 && text[i + 3] != '=')) {
                    return invalid("misplaced base64 padding", i + k);
                }
                ++padding;
                v <<= 6;
                continue;
            }
            const std::int8_t d = kDecode[static_cast<unsigned char>(c)];
            if (d < 0) {
                return invalid("invalid base64 character", i + k);
            }
            v = v << 6 | static_cast<std::uint32_t>(d);
        }
        // Non-canonical encodings (non-zero unused bits) are rejected.
        if ((padding == 1 && (v & 0xFF) != 0) || (padding == 2 && (v & 0xFFFF) != 0)) {
            return invalid("non-canonical base64", i);
        }
        out.push_back(static_cast<std::byte>(v >> 16));
        if (padding < 2) {
            out.push_back(static_cast<std::byte>(v >> 8 & 0xFF));
        }
        if (padding < 1) {
            out.push_back(static_cast<std::byte>(v & 0xFF));
        }
    }
    return out;
}

} // namespace cfw
