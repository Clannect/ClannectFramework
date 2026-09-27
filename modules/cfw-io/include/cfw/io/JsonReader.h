#pragma once

#include <cstddef>

#include "cfw/core/Result.h"
#include "cfw/core/String.h"
#include "cfw/io/Json.h"

namespace cfw {

// Hard limits for untrusted JSON (scene files from other creators, Figma
// responses, network messages). Exceeding one is a LimitExceeded error, never
// a crash or an unbounded allocation.
struct JsonLimits {
    // Nesting of arrays and objects. 1024 matches what the Qt build accepted,
    // so every scene it could load, CFW can load.
    std::size_t maxDepth = 1024;
    std::size_t maxBytes = 256u * 1024u * 1024u;
    std::size_t maxStringBytes = 64u * 1024u * 1024u;
};

// Parses a JSON document (RFC 8259) and returns its value.
//
// Accepted beyond the strict grammar, because the Qt build accepted it and
// existing files may contain it: a leading UTF-8 byte-order mark, and raw
// control characters inside strings.
//
// Handled defensively: invalid UTF-8 is an error; a lone surrogate escape
// ("\ud800", which cannot exist in UTF-8) becomes U+FFFD; duplicate keys keep
// the last value; integers too large for int64 become doubles; numbers too
// large for a double are an error.
//
// On failure the Error is ParseError (or LimitExceeded) with "offset" (byte),
// "line" and "column" (1-based, column in bytes) context.
//
// Threads: any. Allocates: the resulting value. Uses an explicit stack, so
// deep documents cannot overflow the thread's stack.
[[nodiscard]] Result<JsonValue> parseJson(StringView text, const JsonLimits &limits = {});

} // namespace cfw
