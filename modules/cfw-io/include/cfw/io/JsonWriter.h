#pragma once

#include "cfw/core/String.h"
#include "cfw/io/Json.h"

namespace cfw {

enum class JsonFormat {
    // 4-space indentation, one member or element per line, a newline after the
    // document. The scene-file format.
    Indented,
    // No whitespace at all. For network messages and tokens.
    Compact,
};

// Writes `value` as JSON text, byte-for-byte as the Qt build wrote it, so a
// scene file loaded and saved without changes is identical on disk:
//
// - object members in key order (UTF-16 code-unit order; see JsonObject);
// - `"key": value` with one space after the colon (Indented);
// - empty containers written as "[" newline indent "]" (Indented);
// - integers, and doubles with an integral value of magnitude up to 2^53, as
//   plain integers (-0 as 0);
// - other doubles as their shortest round-trip digits, in plain or exponent
//   form, whichever is shorter (plain on a tie), with at least two exponent
//   digits ("1e-07", "1e+21");
// - NaN and infinity (which JSON cannot express) as null;
// - strings as UTF-8 with only '"', '\\' and control characters escaped:
//   \b \f \n \r \t, and \u00XX (lowercase hex) for the rest.
//
// Threads: any. Allocates: the result.
[[nodiscard]] String writeJson(const JsonValue &value, JsonFormat format = JsonFormat::Indented);

// Appends one number in the format above. Exposed for binary-to-text tools
// and tests.
void appendJsonNumber(String &out, double value);

} // namespace cfw
