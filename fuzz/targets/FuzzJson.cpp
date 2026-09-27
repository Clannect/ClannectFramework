// JSON reader: scene files, settings and Figma responses are all untrusted.
// Whatever parses must write and re-parse to the same value.

#include "cfw/io/JsonReader.h"
#include "cfw/io/JsonWriter.h"

#include <cstdlib>

#include "FuzzTarget.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const cfw::StringView text(reinterpret_cast<const char *>(data), size);
    cfw::JsonLimits limits;
    limits.maxDepth = 64; // keeps deep-nesting inputs cheap; the limit itself is what is tested
    const cfw::Result<cfw::JsonValue> parsed = cfw::parseJson(text, limits);
    if (!parsed) {
        return 0;
    }
    const cfw::Result<cfw::JsonValue> again = cfw::parseJson(cfw::writeJson(parsed.value()), limits);
    if (!again || !(again.value() == parsed.value())) {
        std::abort(); // a round-trip mismatch is a bug, not a rejection
    }
    return 0;
}
