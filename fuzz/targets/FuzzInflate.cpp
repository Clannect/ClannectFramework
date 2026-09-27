// zlib/DEFLATE: PNG data, WebP and any compressed asset. Whatever inflates
// must deflate and inflate back to the same bytes.

#include "cfw/core/Deflate.h"

#include <cstdlib>

#include "FuzzTarget.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const cfw::Span<const std::byte> input(reinterpret_cast<const std::byte *>(data), size);
    constexpr cfw::DecompressLimits limits{1u << 20};
    (void)cfw::inflate(input, limits);
    const cfw::Result<std::vector<std::byte>> out = cfw::zlibDecompress(input, limits);
    if (!out) {
        return 0;
    }
    if (out.value().size() > limits.maxOutputBytes) {
        std::abort();
    }
    const std::vector<std::byte> again = cfw::zlibCompress(out.value(), {static_cast<int>(size % 10)});
    const cfw::Result<std::vector<std::byte>> back = cfw::zlibDecompress(again, limits);
    if (!back || back.value() != out.value()) {
        std::abort(); // our own output must round-trip
    }
    return 0;
}
