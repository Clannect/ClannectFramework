#include "cfw/io/Path.h"

namespace cfw {

namespace {

// std::filesystem speaks char8_t for UTF-8; CFW speaks char. Same bytes.
std::u8string_view asU8(StringView text) {
    return {reinterpret_cast<const char8_t *>(text.data()), text.size()};
}

String fromU8(const std::u8string &text) { return String(reinterpret_cast<const char *>(text.data()), text.size()); }

} // namespace

Path::Path(StringView utf8) : m_path(asU8(utf8)) {}

String Path::toString() const { return fromU8(m_path.generic_u8string()); }
String Path::fileName() const { return fromU8(m_path.filename().u8string()); }
String Path::stem() const { return fromU8(m_path.stem().u8string()); }
String Path::extension() const { return fromU8(m_path.extension().u8string()); }

Path Path::withExtension(StringView extension) const {
    std::filesystem::path copy = m_path;
    copy.replace_extension(asU8(extension));
    return Path(std::move(copy));
}

} // namespace cfw
