#pragma once

#include <cstddef>
#include <cstdint>

namespace cfw {

// Hard limits every decoder checks before it allocates.
// Images come from other creators, so a header claiming 100,000 x 100,000
// pixels must fail at once, not after trying to allocate 40 GB.
//
// The defaults suit the asset cache: a 16 MB file, at most 16384 pixels on a
// side (the largest texture most GPUs accept) and 256 MB decoded.
struct ImageLimits {
    std::size_t maxInputBytes = 16u * 1024u * 1024u;
    std::uint32_t maxWidth = 16384;
    std::uint32_t maxHeight = 16384;
    std::size_t maxDecodedBytes = 256u * 1024u * 1024u; // of the RGBA8 result
    // Longest side over shortest side. A 1 x 16384 strip is legitimate (a
    // gradient); 1 x 16384 at an aspect ratio beyond this is not.
    std::uint32_t maxAspectRatio = 16384;
};

} // namespace cfw
