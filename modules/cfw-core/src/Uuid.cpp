#include "cfw/core/Uuid.h"

#include <random>

#include "cfw/core/Hash.h"

namespace cfw {

namespace {

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

Uuid Uuid::generate() {
    // A local random_device reads the OS entropy source (rand_s / getrandom);
    // no shared generator state exists between calls or threads.
    std::random_device entropy;
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        const std::uint32_t word = entropy();
        bytes[i] = static_cast<std::uint8_t>(word);
        bytes[i + 1] = static_cast<std::uint8_t>(word >> 8);
        bytes[i + 2] = static_cast<std::uint8_t>(word >> 16);
        bytes[i + 3] = static_cast<std::uint8_t>(word >> 24);
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40); // version 4
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80); // RFC variant
    return Uuid(bytes);
}

Result<Uuid> Uuid::parse(StringView text) {
    if (text.size() == 38 && text.front() == '{' && text.back() == '}') {
        text = text.substr(1, 36);
    }
    if (text.size() != 36) {
        return Error(ErrorCode::ParseError, "UUID must be 36 characters").with("length", std::to_string(text.size()));
    }
    std::array<std::uint8_t, 16> bytes{};
    std::size_t byte = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') {
                return Error(ErrorCode::ParseError, "UUID hyphen expected").with("offset", std::to_string(i));
            }
            ++i;
            continue;
        }
        const int high = hexValue(text[i]);
        const int low = hexValue(text[i + 1]);
        if (high < 0 || low < 0) {
            return Error(ErrorCode::ParseError, "UUID has a non-hex character").with("offset", std::to_string(i));
        }
        bytes[byte++] = static_cast<std::uint8_t>(high << 4 | low);
        i += 2;
    }
    return Uuid(bytes);
}

String Uuid::toString() const {
    constexpr char kHex[] = "0123456789abcdef";
    String out;
    out.reserve(36);
    for (std::size_t i = 0; i < m_bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out += '-';
        }
        out += kHex[m_bytes[i] >> 4];
        out += kHex[m_bytes[i] & 0x0F];
    }
    return out;
}

std::size_t UuidHash::operator()(const Uuid &id) const noexcept {
    std::uint64_t hash = 0;
    for (std::uint8_t b : id.bytes()) {
        hash = hashCombine(hash, b);
    }
    return static_cast<std::size_t>(hash);
}

} // namespace cfw
