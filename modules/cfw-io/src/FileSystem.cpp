#include "cfw/io/FileSystem.h"

#include <algorithm>
#include <filesystem>

#include "PlatformFile.h"
#include "cfw/core/Utf8.h"

namespace cfw {

namespace fs = std::filesystem;

Result<std::vector<std::byte>> readFile(const Path &path, std::size_t maxBytes) {
    return detail::readWholeFile(path, maxBytes);
}

Result<String> readTextFile(const Path &path, std::size_t maxBytes) {
    Result<std::vector<std::byte>> bytes = readFile(path, maxBytes);
    if (!bytes) {
        return std::move(bytes).error();
    }
    const std::vector<std::byte> &data = bytes.value();
    StringView text(reinterpret_cast<const char *>(data.data()), data.size());
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    const std::size_t bad = findInvalidUtf8(text);
    if (bad != text.size()) {
        return Error(ErrorCode::ParseError, "file is not valid UTF-8")
            .with("path", path.toString())
            .with("offset", std::to_string(bad));
    }
    return String(text);
}

Result<void> writeFileAtomic(const Path &path, Span<const std::byte> data) { return detail::atomicReplace(path, data); }

Result<void> writeFileAtomic(const Path &path, StringView text) {
    return detail::atomicReplace(path, Span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size()));
}

bool exists(const Path &path) noexcept {
    std::error_code ec;
    return fs::exists(path.native(), ec);
}

bool isDirectory(const Path &path) noexcept {
    std::error_code ec;
    return fs::is_directory(path.native(), ec);
}

bool isFile(const Path &path) noexcept {
    std::error_code ec;
    return fs::is_regular_file(path.native(), ec);
}

Result<std::uint64_t> fileSize(const Path &path) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path.native(), ec);
    if (ec) {
        return detail::fileError(ec, "size", path);
    }
    return static_cast<std::uint64_t>(size);
}

Result<std::int64_t> lastWriteTime(const Path &path) {
    std::error_code ec;
    const fs::file_time_type time = fs::last_write_time(path.native(), ec);
    if (ec) {
        return detail::fileError(ec, "stat", path);
    }
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
}

Result<void> createDirectories(const Path &path) {
    std::error_code ec;
    fs::create_directories(path.native(), ec);
    if (ec) {
        return detail::fileError(ec, "create directory", path);
    }
    return success();
}

Result<void> removeFile(const Path &path) {
    std::error_code ec;
    fs::remove(path.native(), ec);
    if (ec) {
        return detail::fileError(ec, "remove", path);
    }
    return success();
}

Result<void> removeAll(const Path &path) {
    std::error_code ec;
    fs::remove_all(path.native(), ec);
    if (ec) {
        return detail::fileError(ec, "remove directory tree", path);
    }
    return success();
}

Result<void> copyFile(const Path &from, const Path &to) {
    std::error_code ec;
    fs::copy_file(from.native(), to.native(), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return detail::fileError(ec, "copy", from).with("to", to.toString());
    }
    return success();
}

Result<void> renameFile(const Path &from, const Path &to) {
    std::error_code ec;
    fs::rename(from.native(), to.native(), ec);
    if (ec) {
        return detail::fileError(ec, "rename", from).with("to", to.toString());
    }
    return success();
}

Result<std::vector<DirectoryEntry>> listDirectory(const Path &path) {
    std::error_code ec;
    fs::directory_iterator it(path.native(), ec);
    if (ec) {
        return detail::fileError(ec, "list directory", path);
    }
    std::vector<DirectoryEntry> entries;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) {
            return detail::fileError(ec, "list directory", path);
        }
        DirectoryEntry entry;
        entry.path = Path(it->path());
        std::error_code statError;
        entry.isDirectory = it->is_directory(statError);
        if (!entry.isDirectory) {
            const std::uintmax_t size = it->file_size(statError);
            entry.size = statError ? 0 : static_cast<std::uint64_t>(size);
        }
        entries.push_back(std::move(entry));
    }
    if (ec) {
        return detail::fileError(ec, "list directory", path);
    }
    std::sort(entries.begin(), entries.end(), [](const DirectoryEntry &a, const DirectoryEntry &b) { return a.path < b.path; });
    return entries;
}

} // namespace cfw
