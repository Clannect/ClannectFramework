#pragma once

#include <cstddef>
#include <utility>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/io/FileSystem.h"
#include "cfw/io/Path.h"

namespace cfw {

// A read-only memory mapping of a whole file: large assets are read without
// copying them into the heap. Move-only; unmapped on destruction.
//
// The mapped bytes can change if another process modifies the file, so treat
// them as untrusted input exactly like a read buffer.
//
// Threads: the bytes may be read from any thread while the MappedFile lives.
// Allocates: address space, not heap.
class MappedFile {
public:
    MappedFile() = default;
    MappedFile(MappedFile &&other) noexcept;
    MappedFile &operator=(MappedFile &&other) noexcept;
    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;
    ~MappedFile();

    [[nodiscard]] static Result<MappedFile> open(const Path &path, std::size_t maxBytes = kDefaultMaxFileBytes);

    [[nodiscard]] Span<const std::byte> bytes() const noexcept { return {m_data, m_size}; }
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }

private:
    void close() noexcept;

    const std::byte *m_data = nullptr;
    std::size_t m_size = 0;
    void *m_fileHandle = nullptr;
    void *m_mappingHandle = nullptr;
};

} // namespace cfw
