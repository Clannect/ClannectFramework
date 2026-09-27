#pragma once

// Non-cryptographic hashing with a fixed, platform-independent definition, so
// a hash computed at compile time, at runtime, or on another machine agrees.
// Not for security: use cfw::Sha256 for content ids.

#include <cstdint>

#include "cfw/core/String.h"

namespace cfw {

// 64-bit FNV-1a over the bytes of `text`.
[[nodiscard]] constexpr std::uint64_t hashString(StringView text) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (char c : text) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

// Mixes `value` into `seed` (boost::hash_combine's 64-bit form).
[[nodiscard]] constexpr std::uint64_t hashCombine(std::uint64_t seed, std::uint64_t value) noexcept {
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 12) + (seed >> 4));
}

} // namespace cfw
