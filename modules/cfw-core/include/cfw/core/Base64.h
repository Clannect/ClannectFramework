#pragma once

// Base64 (RFC 4648 §4, standard alphabet, with '=' padding).
//
// Threads: any (pure functions). Allocates: the result.

#include <cstddef>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"

namespace cfw {

[[nodiscard]] String base64Encode(Span<const std::byte> data);
[[nodiscard]] String base64Encode(StringView text);

// Strict: the length must be a multiple of 4, padding only at the end, only
// alphabet characters, and unused bits in the last group must be zero (so each
// byte string has exactly one encoding). No whitespace.
[[nodiscard]] Result<std::vector<std::byte>> base64Decode(StringView text);

} // namespace cfw
