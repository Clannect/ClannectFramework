#include "cfw/core/Sha1.h"

#include <algorithm>
#include <cstring>

namespace cfw {

namespace {

constexpr std::uint32_t rotl(std::uint32_t x, int n) noexcept { return (x << n) | (x >> (32 - n)); }

} // namespace

Sha1::Sha1() noexcept : m_state{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0} {}

void Sha1::compress(const std::uint8_t *block) noexcept {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = static_cast<std::uint32_t>(block[i * 4]) << 24 | static_cast<std::uint32_t>(block[i * 4 + 1]) << 16 |
               static_cast<std::uint32_t>(block[i * 4 + 2]) << 8 | static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3], e = m_state[4];
    for (int i = 0; i < 80; ++i) {
        std::uint32_t f = 0;
        std::uint32_t k = 0;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = t;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
}

void Sha1::update(Span<const std::byte> data) noexcept {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(data.data());
    std::size_t remaining = data.size();
    m_totalBytes += remaining;
    if (m_buffered > 0) {
        const std::size_t take = std::min(remaining, m_buffer.size() - m_buffered);
        std::memcpy(m_buffer.data() + m_buffered, bytes, take);
        m_buffered += take;
        bytes += take;
        remaining -= take;
        if (m_buffered < m_buffer.size()) {
            return;
        }
        compress(m_buffer.data());
        m_buffered = 0;
    }
    while (remaining >= 64) {
        compress(bytes);
        bytes += 64;
        remaining -= 64;
    }
    if (remaining > 0) {
        std::memcpy(m_buffer.data(), bytes, remaining);
        m_buffered = remaining;
    }
}

void Sha1::update(StringView text) noexcept {
    update(Span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size()));
}

Sha1::Digest Sha1::finish() noexcept {
    const std::uint64_t bitLength = m_totalBytes * 8;
    m_buffer[m_buffered++] = 0x80;
    if (m_buffered > 56) {
        std::memset(m_buffer.data() + m_buffered, 0, m_buffer.size() - m_buffered);
        compress(m_buffer.data());
        m_buffered = 0;
    }
    std::memset(m_buffer.data() + m_buffered, 0, 56 - m_buffered);
    for (int i = 0; i < 8; ++i) {
        m_buffer[static_cast<std::size_t>(56 + i)] = static_cast<std::uint8_t>(bitLength >> (56 - 8 * i));
    }
    compress(m_buffer.data());
    Digest digest{};
    for (std::size_t i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(m_state[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(m_state[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(m_state[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(m_state[i]);
    }
    return digest;
}

Sha1::Digest Sha1::hash(StringView text) noexcept {
    Sha1 sha;
    sha.update(text);
    return sha.finish();
}

} // namespace cfw
