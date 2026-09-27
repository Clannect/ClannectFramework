#include "cfw/io/TemporaryDirectory.h"

#include <filesystem>

#include "PlatformFile.h"
#include "cfw/core/Uuid.h"
#include "cfw/io/FileSystem.h"

namespace cfw {

Result<TemporaryDirectory> TemporaryDirectory::create(StringView prefix) {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return detail::fileError(ec, "find temporary directory", Path());
    }
    // A random name, created exclusively: retry on the (vanishingly unlikely) clash.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const Path candidate = Path(base) / (String(prefix) + Uuid::generate().toString());
        if (std::filesystem::create_directory(candidate.native(), ec)) {
            return TemporaryDirectory(candidate);
        }
        if (ec) {
            return detail::fileError(ec, "create temporary directory", candidate);
        }
    }
    return Error(ErrorCode::AlreadyExists, "could not create a unique temporary directory");
}

TemporaryDirectory::TemporaryDirectory(TemporaryDirectory &&other) noexcept
    : m_path(std::move(other.m_path)), m_keep(other.m_keep) {
    other.m_path = Path();
}

TemporaryDirectory &TemporaryDirectory::operator=(TemporaryDirectory &&other) noexcept {
    if (this != &other) {
        if (!m_keep && !m_path.empty()) {
            (void)removeAll(m_path);
        }
        m_path = std::move(other.m_path);
        m_keep = other.m_keep;
        other.m_path = Path();
    }
    return *this;
}

TemporaryDirectory::~TemporaryDirectory() {
    if (!m_keep && !m_path.empty()) {
        (void)removeAll(m_path); // best effort: a destructor cannot report failure
    }
}

} // namespace cfw
