#include "cfw/io/Zip.h"

#include <algorithm>

#include "cfw/core/Checksum.h"
#include "cfw/core/Deflate.h"
#include "cfw/core/Utf8.h"
#include "cfw/io/FileSystem.h"

namespace cfw {

namespace {

constexpr std::uint32_t kLocalHeader = 0x04034b50;
constexpr std::uint32_t kCentralHeader = 0x02014b50;
constexpr std::uint32_t kEndOfCentralDirectory = 0x06054b50;
constexpr std::uint32_t kZip64Locator = 0x07064b50;
constexpr std::size_t kEocdSize = 22;
constexpr std::size_t kCentralSize = 46;
constexpr std::size_t kLocalSize = 30;

Error corrupt(const char *what) { return Error(ErrorCode::Corrupt, String("zip: ") + what); }

std::uint16_t u16(Span<const std::byte> d, std::size_t at) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(d[at]) | std::to_integer<unsigned>(d[at + 1]) << 8);
}

std::uint32_t u32(Span<const std::byte> d, std::size_t at) noexcept {
    return static_cast<std::uint32_t>(u16(d, at)) | static_cast<std::uint32_t>(u16(d, at + 2)) << 16;
}

} // namespace

Result<ZipArchive> ZipArchive::open(Span<const std::byte> data, const ZipLimits &limits) {
    // The end of central directory record: the last one, searched backwards
    // over at most the 64 KB comment that may follow it.
    if (data.size() < kEocdSize) {
        return corrupt("too short for an archive");
    }
    std::size_t eocd = data.size();
    const std::size_t lowest = data.size() > kEocdSize + 0xFFFF ? data.size() - kEocdSize - 0xFFFF : 0;
    for (std::size_t at = data.size() - kEocdSize + 1; at-- > lowest;) {
        if (u32(data, at) == kEndOfCentralDirectory && at + kEocdSize + u16(data, at + 20) == data.size()) {
            eocd = at;
            break;
        }
    }
    if (eocd == data.size()) {
        return corrupt("no end of central directory record");
    }
    if (eocd >= 20 && u32(data, eocd - 20) == kZip64Locator) {
        return Error(ErrorCode::Unsupported, "zip: zip64 archives are not supported");
    }
    const std::uint16_t disk = u16(data, eocd + 4);
    const std::uint16_t cdDisk = u16(data, eocd + 6);
    const std::uint16_t countHere = u16(data, eocd + 8);
    const std::uint16_t count = u16(data, eocd + 10);
    const std::uint32_t cdSize = u32(data, eocd + 12);
    const std::uint32_t cdOffset = u32(data, eocd + 16);
    if (disk != 0 || cdDisk != 0 || countHere != count) {
        return Error(ErrorCode::Unsupported, "zip: multi-part archives are not supported");
    }
    if (count == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu) {
        return Error(ErrorCode::Unsupported, "zip: zip64 archives are not supported");
    }
    if (count > limits.maxEntries) {
        return Error(ErrorCode::LimitExceeded, "zip: too many entries");
    }
    // The central directory ends where the record starts; anything before
    // the archive's own offset 0 is a prefix (an executable, say).
    if (cdSize > eocd || cdOffset > eocd - cdSize) {
        return corrupt("central directory outside the file");
    }
    const std::size_t cdStart = eocd - cdSize;
    const std::size_t base = cdStart - cdOffset;

    ZipArchive archive;
    archive.m_data = data.subspan(base);
    const Span<const std::byte> cd = data.subspan(cdStart, cdSize);
    archive.m_entries.reserve(count);
    std::size_t at = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (cd.size() - at < kCentralSize || u32(cd, at) != kCentralHeader) {
            return corrupt("bad central directory entry");
        }
        const std::uint16_t flags = u16(cd, at + 8);
        ZipEntry e;
        e.method = u16(cd, at + 10);
        e.crc32 = u32(cd, at + 16);
        e.compressedSize = u32(cd, at + 20);
        e.size = u32(cd, at + 24);
        const std::size_t nameLength = u16(cd, at + 28);
        const std::size_t extraLength = u16(cd, at + 30);
        const std::size_t commentLength = u16(cd, at + 32);
        e.localHeaderOffset = u32(cd, at + 42);
        const std::size_t recordSize = kCentralSize + nameLength + extraLength + commentLength;
        if (cd.size() - at < recordSize) {
            return corrupt("central directory entry past its end");
        }
        e.name.assign(reinterpret_cast<const char *>(cd.data() + at + kCentralSize), nameLength);
        at += recordSize;

        if ((flags & 1) != 0) {
            return Error(ErrorCode::Unsupported, "zip: encrypted entries are not supported");
        }
        if (e.method != 0 && e.method != 8) {
            return Error(ErrorCode::Unsupported, "zip: compression method " + std::to_string(e.method) +
                                                     " is not supported");
        }
        if (e.method == 0 && e.compressedSize != e.size) {
            return corrupt("a stored entry whose sizes differ");
        }
        if (e.size > limits.maxEntryBytes) {
            return Error(ErrorCode::LimitExceeded, "zip: an entry is too large");
        }
        archive.m_totalSize += e.size;
        if (archive.m_totalSize > limits.maxTotalBytes) {
            return Error(ErrorCode::LimitExceeded, "zip: the entries are too large together");
        }
        e.isDirectory = !e.name.empty() && e.name.back() == '/';
        archive.m_entries.push_back(std::move(e));
    }
    return archive;
}

const ZipEntry *ZipArchive::find(StringView name) const noexcept {
    for (const ZipEntry &e : m_entries) {
        if (e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

Result<std::vector<std::byte>> ZipArchive::read(const ZipEntry &e) const {
    const std::uint64_t at = e.localHeaderOffset;
    if (at > m_data.size() || m_data.size() - at < kLocalSize || u32(m_data, at) != kLocalHeader) {
        return corrupt("bad local header");
    }
    const std::size_t nameLength = u16(m_data, at + 26);
    const std::size_t extraLength = u16(m_data, at + 28);
    if (u16(m_data, at + 8) != e.method) {
        return corrupt("local header disagrees with the central directory");
    }
    const std::uint64_t start = at + kLocalSize + nameLength + extraLength;
    if (start > m_data.size() || m_data.size() - start < e.compressedSize) {
        return corrupt("entry data past the end of the archive");
    }
    const Span<const std::byte> packed = m_data.subspan(start, e.compressedSize);
    std::vector<std::byte> out;
    if (e.method == 0) {
        out.assign(packed.begin(), packed.end());
    } else {
        Result<std::vector<std::byte>> inflated = inflate(packed, DecompressLimits{e.size});
        if (!inflated) {
            return inflated.error().code() == ErrorCode::LimitExceeded ? corrupt("an entry is larger than declared")
                                                                       : inflated.error();
        }
        out = std::move(inflated.value());
    }
    if (out.size() != e.size) {
        return corrupt("an entry is smaller than declared");
    }
    if (crc32(out) != e.crc32) {
        return corrupt("CRC-32 mismatch");
    }
    return out;
}

Result<Path> zipEntryRelativePath(StringView name) {
    const auto bad = [&](const char *why) {
        return Error(ErrorCode::InvalidArgument, "zip: unsafe entry name \"" + String(name) + "\": " + why);
    };
    if (name.empty()) {
        return bad("empty");
    }
    if (!isValidUtf8(name)) {
        return bad("not UTF-8");
    }
    if (name.front() == '/') {
        return bad("absolute");
    }
    if (name.find('\\') != StringView::npos || name.find(':') != StringView::npos ||
        name.find('\0') != StringView::npos) {
        return bad("backslash, colon or NUL");
    }
    String relative;
    std::size_t start = 0;
    while (start <= name.size()) {
        std::size_t end = name.find('/', start);
        if (end == StringView::npos) {
            end = name.size();
        }
        const StringView part = name.substr(start, end - start);
        if (part == "..") {
            return bad("goes up with ..");
        }
        if (!part.empty() && part != ".") {
            if (!relative.empty()) {
                relative += '/';
            }
            relative += part;
        }
        start = end + 1;
    }
    if (relative.empty()) {
        return bad("names no file");
    }
    return Path(relative);
}

Result<void> extractZip(const ZipArchive &archive, const Path &destination, const ExtractOptions &options) {
    if (Result<void> made = createDirectories(destination); !made) {
        return made;
    }
    std::uint64_t done = 0;
    for (const ZipEntry &e : archive.entries()) {
        // Drop the leading folders first, then check what is left.
        StringView name = e.name;
        int strip = options.stripComponents;
        while (strip > 0) {
            const std::size_t slash = name.find('/');
            if (slash == StringView::npos) {
                break;
            }
            name.remove_prefix(slash + 1);
            --strip;
        }
        if (strip > 0 || name.empty() || name == "/") {
            continue; // the stripped folders themselves
        }
        Result<Path> relative = zipEntryRelativePath(name);
        if (!relative) {
            return relative.error();
        }
        const Path target = destination / relative.value();
        if (e.isDirectory) {
            if (Result<void> made = createDirectories(target); !made) {
                return made;
            }
            continue;
        }
        Result<std::vector<std::byte>> bytes = archive.read(e);
        if (!bytes) {
            return Error(bytes.error().code(), bytes.error().message() + " (" + e.name + ")");
        }
        if (Result<void> made = createDirectories(target.parent()); !made) {
            return made;
        }
        if (Result<void> written = writeFileAtomic(target, bytes.value()); !written) {
            return written;
        }
        done += e.size;
        if (options.progress && !options.progress(done, archive.totalSize())) {
            return Error(ErrorCode::Cancelled, "zip: extraction cancelled");
        }
    }
    return {};
}

} // namespace cfw
