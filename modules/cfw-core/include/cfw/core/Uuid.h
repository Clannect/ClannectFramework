#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <ostream>

#include "cfw/core/Result.h"
#include "cfw/core/String.h"

namespace cfw {

// A 128-bit universally unique identifier (RFC 9562).
//
// Text form is lowercase "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" without braces,
// the form the engine already writes; parse() also accepts uppercase and
// surrounding braces, so ids saved in either form still load.
//
// Threads: a value type; generate() may be called from any thread.
// Allocates: only toString().
class Uuid {
public:
    // The nil UUID (all zeros).
    constexpr Uuid() noexcept = default;
    explicit constexpr Uuid(const std::array<std::uint8_t, 16> &bytes) noexcept : m_bytes(bytes) {}

    // A random (version 4) UUID from the operating system's entropy source.
    [[nodiscard]] static Uuid generate();
    [[nodiscard]] static Result<Uuid> parse(StringView text);

    [[nodiscard]] String toString() const;
    [[nodiscard]] constexpr bool isNil() const noexcept { return *this == Uuid(); }
    [[nodiscard]] constexpr int version() const noexcept { return m_bytes[6] >> 4; }
    [[nodiscard]] constexpr const std::array<std::uint8_t, 16> &bytes() const noexcept { return m_bytes; }

    friend constexpr auto operator<=>(const Uuid &a, const Uuid &b) noexcept = default;

    friend std::ostream &operator<<(std::ostream &out, const Uuid &id) { return out << id.toString(); }

private:
    std::array<std::uint8_t, 16> m_bytes{};
};

struct UuidHash {
    [[nodiscard]] std::size_t operator()(const Uuid &id) const noexcept;
};

} // namespace cfw
