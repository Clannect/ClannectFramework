#pragma once

// File and directory operations. Every function is fallible and returns a
// Result: IoError, NotFound, PermissionDenied, AlreadyExists or LimitExceeded,
// with the path in the error's context. Nothing throws.
//
// Threads: any; operations on different files are independent. Concurrent
// writes to the same file are the caller's problem, as with any file API.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"
#include "cfw/io/Path.h"

namespace cfw {

// Reading refuses files larger than this unless the caller passes a limit.
inline constexpr std::size_t kDefaultMaxFileBytes = 256u * 1024u * 1024u;

[[nodiscard]] Result<std::vector<std::byte>> readFile(const Path &path,
                                                      std::size_t maxBytes = kDefaultMaxFileBytes);
// Reads a text file. A UTF-8 byte-order mark is dropped; the rest must be
// valid UTF-8 (ParseError with the byte offset otherwise).
[[nodiscard]] Result<String> readTextFile(const Path &path, std::size_t maxBytes = kDefaultMaxFileBytes);

// Replaces the file's contents atomically: the data goes to a temporary file
// in the same directory, is flushed to the disk, and is then renamed over the
// target. A crash, power loss or full disk at any point leaves either the old
// file or the new one, never a truncated mix. The parent directory must exist.
//
// The temporary file is the target's sibling ("<name>.cfw-tmp-<random>"),
// never a file in the system's temporary directory: the final rename stays
// inside one directory, so it is atomic wherever the target is, including a
// volume other than the one that holds the temporary directory (a project on
// D: with TEMP on C:, a save on a USB stick, a network share). The system's
// temporary directory is not used and need not exist. An interrupted write
// can leave the sibling behind; it is never mistaken for the file.
//
// A target that is a symbolic link is replaced by a regular file: the link
// itself is renamed over, not followed.
[[nodiscard]] Result<void> writeFileAtomic(const Path &path, Span<const std::byte> data);
[[nodiscard]] Result<void> writeFileAtomic(const Path &path, StringView text);

[[nodiscard]] bool exists(const Path &path) noexcept;
[[nodiscard]] bool isDirectory(const Path &path) noexcept;
[[nodiscard]] bool isFile(const Path &path) noexcept;
[[nodiscard]] Result<std::uint64_t> fileSize(const Path &path);
// Modification time in nanoseconds since an unspecified epoch: only for
// comparing with other values from this function.
[[nodiscard]] Result<std::int64_t> lastWriteTime(const Path &path);

// Creates the directory and any missing parents. Succeeds if it exists.
[[nodiscard]] Result<void> createDirectories(const Path &path);
// Removes a file or an empty directory. Succeeds if nothing is there.
[[nodiscard]] Result<void> removeFile(const Path &path);
// Removes a directory tree. Succeeds if nothing is there.
[[nodiscard]] Result<void> removeAll(const Path &path);
[[nodiscard]] Result<void> copyFile(const Path &from, const Path &to);
// Renames within one volume, replacing an existing target.
[[nodiscard]] Result<void> renameFile(const Path &from, const Path &to);

struct DirectoryEntry {
    Path path;
    bool isDirectory = false;
    std::uint64_t size = 0;
};

// The directory's direct children, sorted by path so results are
// deterministic. Symlinks are reported, not followed.
[[nodiscard]] Result<std::vector<DirectoryEntry>> listDirectory(const Path &path);

} // namespace cfw
