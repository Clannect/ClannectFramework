// URLs and text encodings: asset links and Figma URLs come from other
// creators. Also UTF-8 validation and UTF-16 conversion, used on every string
// that crosses a boundary.

#include "cfw/core/Url.h"
#include "cfw/core/Utf8.h"

#include <cstdlib>

#include "FuzzTarget.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const cfw::StringView text(reinterpret_cast<const char *>(data), size);

    if (cfw::Result<cfw::Url> url = cfw::Url::parse(text)) {
        (void)url.value().toLocalPath();
        (void)url.value().resolve(text);
        (void)url.value().resolve("../a/./b?q#f");
    }
    (void)cfw::percentDecode(text);

    const bool valid = cfw::isValidUtf8(text);
    if (valid != (cfw::findInvalidUtf8(text) == text.size())) {
        std::abort();
    }
    if (valid) {
        cfw::Result<std::u16string> wide = cfw::utf8ToUtf16(text);
        if (!wide) {
            std::abort();
        }
        cfw::Result<cfw::String> back = cfw::utf16ToUtf8(wide.value());
        if (!back || back.value() != text) {
            std::abort(); // valid UTF-8 must survive a UTF-16 round trip exactly
        }
    }
    if (!cfw::isValidUtf8(cfw::sanitizeUtf8(text))) {
        std::abort();
    }
    return 0;
}
