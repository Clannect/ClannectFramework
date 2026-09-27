#pragma once

// Private to cfw-io: the few file operations that need the OS directly, so
// errors are precise and "flushed to disk" is real. Implemented once per
// platform (PlatformFileWin32.cpp, PlatformFilePosix.cpp). This header itself
// includes no platform headers.

#include <cstddef>
#include <system_error>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/io/Path.h"

namespace cfw::detail {

// Maps an OS or std error to a CFW error with the path attached.
[[nodiscard]] Error fileError(const std::error_code &code, StringView action, const Path &path);

[[nodiscard]] Result<std::vector<std::byte>> readWholeFile(const Path &path, std::size_t maxBytes);

// Writes `data` to a new temporary file next to `path`, flushes it to stable
// storage, and renames it over `path` (flushing the directory where the OS
// needs that). Removes the temporary file on any failure.
[[nodiscard]] Result<void> atomicReplace(const Path &path, Span<const std::byte> data);

// Read-only memory mapping. `handle` values are opaque to callers.
struct Mapping {
    const std::byte *data = nullptr;
    std::size_t size = 0;
    void *fileHandle = nullptr;
    void *mappingHandle = nullptr;
};
[[nodiscard]] Result<Mapping> mapFile(const Path &path, std::size_t maxBytes);
void unmapFile(Mapping &mapping) noexcept;

} // namespace cfw::detail
