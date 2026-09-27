#pragma once

// CRC-32 (ISO 3309 / ITU-T V.42, the one PNG, zip and gzip use) and Adler-32
// (RFC 1950, the zlib stream checksum). Both are incremental: pass the
// previous result back in to continue a running checksum.
//
// Threads: any (pure functions). Allocates: nothing.

#include <cstddef>
#include <cstdint>

#include "cfw/core/Span.h"

namespace cfw {

// crc32(data) starts a checksum; crc32(more, previous) continues it.
[[nodiscard]] std::uint32_t crc32(Span<const std::byte> data, std::uint32_t previous = 0) noexcept;

// adler32(data) starts a checksum (the initial value is 1); adler32(more,
// previous) continues it.
[[nodiscard]] std::uint32_t adler32(Span<const std::byte> data, std::uint32_t previous = 1) noexcept;

} // namespace cfw
