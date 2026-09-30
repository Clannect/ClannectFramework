#pragma once

// Reading zip archives (PKWARE APPNOTE 6.3): the installer's packages, and
// any zip an application is handed. Stored and DEFLATE entries; not zip64,
// encryption or other methods (those fail with Unsupported).
//
// Archives are untrusted input. The central directory is the index (as in
// every mainstream reader), each entry's local header is checked against it,
// sizes are bounded by ZipLimits, an entry inflates to exactly its declared
// size and its CRC-32 must match. extractZip() writes only below its
// destination: absolute names, drive letters, ".." and names that are not
// UTF-8 are rejected before anything is written.
//
// Threads: an archive may be read from several threads (it is immutable).
// Allocates: the entry table, and each entry's bytes when read.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"
#include "cfw/io/Path.h"

namespace cfw {

struct ZipLimits {
    std::size_t maxEntries = 100000;
    // Per entry, and all entries together.
    std::uint64_t maxEntryBytes = 1ull << 30;
    std::uint64_t maxTotalBytes = 4ull << 30;
};

struct ZipEntry {
    String name;                 // as stored: '/'-separated UTF-8
    bool isDirectory = false;    // the name ends in '/'
    std::uint16_t method = 0;    // 0 stored, 8 DEFLATE
    std::uint32_t crc32 = 0;
    std::uint64_t compressedSize = 0;
    std::uint64_t size = 0;
    std::uint64_t localHeaderOffset = 0;
};

class ZipArchive {
public:
    // The data is borrowed and must outlive the archive. A zip may follow
    // other bytes (an installer's executable, say): offsets are relative to
    // where the archive starts, which is found from its end.
    [[nodiscard]] static Result<ZipArchive> open(Span<const std::byte> data, const ZipLimits &limits = {});

    [[nodiscard]] const std::vector<ZipEntry> &entries() const noexcept { return m_entries; }
    // The first entry with this name, or null.
    [[nodiscard]] const ZipEntry *find(StringView name) const noexcept;
    // The entry's contents: inflated, size- and CRC-checked.
    [[nodiscard]] Result<std::vector<std::byte>> read(const ZipEntry &entry) const;
    // The sum of the entries' sizes.
    [[nodiscard]] std::uint64_t totalSize() const noexcept { return m_totalSize; }

private:
    Span<const std::byte> m_data; // from the archive's first byte
    std::vector<ZipEntry> m_entries;
    std::uint64_t m_totalSize = 0;
};

// Where `name` goes below a destination folder, as a relative path; fails
// with InvalidArgument for names that would leave it (absolute, "C:",
// "..", backslashes) or that are empty.
[[nodiscard]] Result<Path> zipEntryRelativePath(StringView name);

struct ExtractOptions {
    // Leading folders to drop from every name ("pkg-1.0/lib/x.a" with 1 gives
    // "lib/x.a"); entries with fewer are skipped.
    int stripComponents = 0;
    // Called after each file with the bytes written so far and in all;
    // returning false cancels (the call fails with Cancelled).
    std::function<bool(std::uint64_t done, std::uint64_t total)> progress;
};

// Extracts every entry below `destination` (created if needed). Fails on the
// first bad entry; files written before it are left in place.
[[nodiscard]] Result<void> extractZip(const ZipArchive &archive, const Path &destination,
                                      const ExtractOptions &options = {});

} // namespace cfw
