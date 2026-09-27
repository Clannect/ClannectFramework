#include "cfw/core/Checksum.h"

#include <array>

namespace cfw {

namespace {

// Slicing-by-4 tables for the reflected polynomial 0xEDB88320.
constexpr std::array<std::array<std::uint32_t, 256>, 4> makeCrcTables() {
    std::array<std::array<std::uint32_t, 256>, 4> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        t[0][i] = c;
    }
    for (std::uint32_t i = 0; i < 256; ++i) {
        for (std::size_t s = 1; s < 4; ++s) {
            t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFFu];
        }
    }
    return t;
}

constexpr auto kCrcTables = makeCrcTables();

} // namespace

std::uint32_t crc32(Span<const std::byte> data, std::uint32_t previous) noexcept {
    std::uint32_t c = ~previous;
    const auto *p = reinterpret_cast<const std::uint8_t *>(data.data());
    std::size_t n = data.size();
    while (n >= 4) {
        c ^= static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
             static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
        c = kCrcTables[3][c & 0xFFu] ^ kCrcTables[2][(c >> 8) & 0xFFu] ^ kCrcTables[1][(c >> 16) & 0xFFu] ^
            kCrcTables[0][c >> 24];
        p += 4;
        n -= 4;
    }
    while (n-- > 0) {
        c = kCrcTables[0][(c ^ *p++) & 0xFFu] ^ (c >> 8);
    }
    return ~c;
}

std::uint32_t adler32(Span<const std::byte> data, std::uint32_t previous) noexcept {
    constexpr std::uint32_t kMod = 65521;
    // 5552 is the largest block for which the sums cannot overflow 32 bits.
    constexpr std::size_t kBlock = 5552;
    std::uint32_t a = previous & 0xFFFFu;
    std::uint32_t b = previous >> 16;
    const auto *p = reinterpret_cast<const std::uint8_t *>(data.data());
    std::size_t n = data.size();
    while (n > 0) {
        const std::size_t block = n < kBlock ? n : kBlock;
        for (std::size_t i = 0; i < block; ++i) {
            a += p[i];
            b += a;
        }
        a %= kMod;
        b %= kMod;
        p += block;
        n -= block;
    }
    return b << 16 | a;
}

} // namespace cfw
