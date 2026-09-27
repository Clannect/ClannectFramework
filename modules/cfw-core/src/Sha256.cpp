#include "cfw/core/Sha256.h"

#include <algorithm>
#include <cstring>

#include "cfw/core/Contract.h"

namespace cfw {

namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t x, int n) noexcept { return (x >> n) | (x << (32 - n)); }

} // namespace

Sha256::Sha256() noexcept
    : m_state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::compress(const std::uint8_t *block) noexcept {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = static_cast<std::uint32_t>(block[i * 4]) << 24 | static_cast<std::uint32_t>(block[i * 4 + 1]) << 16 |
               static_cast<std::uint32_t>(block[i * 4 + 2]) << 8 | static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    std::uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kRoundConstants[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
    m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;
}

void Sha256::update(Span<const std::byte> data) noexcept {
    debugCheck(!m_finished, "Sha256::update after finish");
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

void Sha256::update(StringView text) noexcept {
    update(Span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size()));
}

Sha256::Digest Sha256::finish() noexcept {
    debugCheck(!m_finished, "Sha256::finish called twice");
    const std::uint64_t bitLength = m_totalBytes * 8;

    // Padding: 0x80, zeros to 56 mod 64, then the 64-bit big-endian length.
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
    m_finished = true;

    Digest digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(m_state[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(m_state[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(m_state[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(m_state[i]);
    }
    return digest;
}

Sha256::Digest Sha256::hash(Span<const std::byte> data) noexcept {
    Sha256 sha;
    sha.update(data);
    return sha.finish();
}

Sha256::Digest Sha256::hash(StringView text) noexcept {
    Sha256 sha;
    sha.update(text);
    return sha.finish();
}

String Sha256::toHex(const Digest &digest) {
    constexpr char kHex[] = "0123456789abcdef";
    String out;
    out.reserve(64);
    for (std::uint8_t byte : digest) {
        out += kHex[byte >> 4];
        out += kHex[byte & 0x0F];
    }
    return out;
}

} // namespace cfw
