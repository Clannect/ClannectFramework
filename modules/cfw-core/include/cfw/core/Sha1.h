#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "cfw/core/Span.h"
#include "cfw/core/String.h"

namespace cfw {

// SHA-1 (FIPS 180-4). **Not for security**: SHA-1 is broken for collision
// resistance. It exists because protocols require it, e.g. the WebSocket
// handshake's Sec-WebSocket-Accept (RFC 6455 §4.2.2). Use Sha256 for anything
// that must resist tampering.
//
// Threads: one instance per thread. Allocates: nothing.
class Sha1 {
public:
    using Digest = std::array<std::uint8_t, 20>;

    Sha1() noexcept;

    void update(Span<const std::byte> data) noexcept;
    void update(StringView text) noexcept;
    [[nodiscard]] Digest finish() noexcept;

    [[nodiscard]] static Digest hash(StringView text) noexcept;

private:
    void compress(const std::uint8_t *block) noexcept;

    std::array<std::uint32_t, 5> m_state;
    std::array<std::uint8_t, 64> m_buffer{};
    std::size_t m_buffered = 0;
    std::uint64_t m_totalBytes = 0;
};

} // namespace cfw
