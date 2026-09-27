#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "cfw/core/Span.h"
#include "cfw/core/String.h"

namespace cfw {

// SHA-256 (FIPS 180-4). Used for content-addressed asset ids and integrity
// checks, so the output must match every other SHA-256 implementation bit for
// bit: tested against the NIST vectors.
//
// Incremental: call update() any number of times, then finish() once.
//
// Threads: one instance per thread. Allocates: nothing.
class Sha256 {
public:
    using Digest = std::array<std::uint8_t, 32>;

    Sha256() noexcept;

    void update(Span<const std::byte> data) noexcept;
    void update(StringView text) noexcept;
    // Finishes the hash. The object must not be updated afterwards; reuse it
    // by assigning a fresh Sha256.
    [[nodiscard]] Digest finish() noexcept;

    [[nodiscard]] static Digest hash(Span<const std::byte> data) noexcept;
    [[nodiscard]] static Digest hash(StringView text) noexcept;

    // Lowercase hexadecimal, 64 characters.
    [[nodiscard]] static String toHex(const Digest &digest);

private:
    void compress(const std::uint8_t *block) noexcept;

    std::array<std::uint32_t, 8> m_state;
    std::array<std::uint8_t, 64> m_buffer{};
    std::size_t m_buffered = 0;
    std::uint64_t m_totalBytes = 0;
    bool m_finished = false;
};

} // namespace cfw
