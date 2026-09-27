#pragma once

// DEFLATE (RFC 1951) and the zlib stream format around it (RFC 1950), written
// for CFW: PNG, WebP's lossless path and any compressed asset go through here,
// so there is no zlib dependency.
//
// The decoder is built for hostile input: every Huffman table is validated
// (over-subscribed and, except for the single-code case the RFC allows,
// incomplete codes are rejected), back-references before the start of the
// output are errors, and output is capped by DecompressLimits so a small
// "zip bomb" cannot allocate gigabytes. Every failure is a Result, never a
// crash.
//
// The encoder produces standard streams any inflater accepts: LZ77 with hash
// chains and lazy matching, then per block the smallest of stored, fixed and
// dynamic Huffman coding.
//
// Threads: any (pure functions). Allocates: the output, plus the encoder's
// fixed-size working tables (about 400 KB) for the duration of a call.

#include <cstddef>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"

namespace cfw {

struct DecompressLimits {
    // Output beyond this fails with LimitExceeded. Callers that know the
    // exact size (PNG, WebP) pass it.
    std::size_t maxOutputBytes = 256u * 1024u * 1024u;
};

// 0 stores without compressing; 1 is fastest; 9 compresses most. 6 is the
// zlib default and a good balance.
struct CompressOptions {
    int level = 6;
};

// Raw DEFLATE, no header. Fails with Corrupt on a malformed or truncated
// stream and LimitExceeded past the limit. Bytes after the final block are
// ignored.
[[nodiscard]] Result<std::vector<std::byte>> inflate(Span<const std::byte> data, DecompressLimits limits = {});
[[nodiscard]] std::vector<std::byte> deflate(Span<const std::byte> data, CompressOptions options = {});

// zlib streams: a 2-byte header, raw DEFLATE, then the Adler-32 of the output,
// which is checked. Preset dictionaries are Unsupported (PNG forbids them).
[[nodiscard]] Result<std::vector<std::byte>> zlibDecompress(Span<const std::byte> data, DecompressLimits limits = {});
[[nodiscard]] std::vector<std::byte> zlibCompress(Span<const std::byte> data, CompressOptions options = {});

} // namespace cfw
