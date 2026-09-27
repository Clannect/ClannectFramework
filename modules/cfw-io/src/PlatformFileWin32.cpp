// Windows implementation of PlatformFile.h. The only cfw-io file that includes
// <windows.h>.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <random>

#include "PlatformFile.h"

namespace cfw::detail {

namespace {

// Closes a HANDLE on scope exit.
class HandleGuard {
public:
    explicit HandleGuard(HANDLE handle) noexcept : m_handle(handle) {}
    ~HandleGuard() {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
        }
    }
    HandleGuard(const HandleGuard &) = delete;
    HandleGuard &operator=(const HandleGuard &) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }
    HANDLE release() noexcept {
        HANDLE h = m_handle;
        m_handle = nullptr;
        return h;
    }

private:
    HANDLE m_handle;
};

std::error_code lastError() noexcept {
    return std::error_code(static_cast<int>(GetLastError()), std::system_category());
}

// A temporary file name in the same directory, so the final rename never
// crosses volumes: "<name>.cfw-tmp-<random>".
std::wstring temporarySibling(const Path &path) {
    std::random_device entropy;
    std::uint32_t bits = entropy();
    std::wstring suffix = L".cfw-tmp-";
    for (int i = 0; i < 8; ++i) {
        suffix += L"0123456789abcdef"[(bits >> 28) & 0xF];
        bits <<= 4;
    }
    return path.native().wstring() + suffix;
}

} // namespace

Error fileError(const std::error_code &code, StringView action, const Path &path) {
    ErrorCode ce = ErrorCode::IoError;
    if (code.category() == std::system_category()) {
        switch (static_cast<DWORD>(code.value())) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
            ce = ErrorCode::NotFound;
            break;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_WRITE_PROTECT:
            ce = ErrorCode::PermissionDenied;
            break;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            ce = ErrorCode::AlreadyExists;
            break;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            ce = ErrorCode::OutOfMemory;
            break;
        default:
            break;
        }
    }
    if (ce == ErrorCode::IoError) {
        if (code == std::errc::no_such_file_or_directory) {
            ce = ErrorCode::NotFound;
        } else if (code == std::errc::permission_denied) {
            ce = ErrorCode::PermissionDenied;
        } else if (code == std::errc::file_exists) {
            ce = ErrorCode::AlreadyExists;
        }
    }
    String message(action);
    message += " failed: ";
    message += code.message();
    return Error(ce, std::move(message)).with("path", path.toString()).with("os-error", std::to_string(code.value()));
}

Result<std::vector<std::byte>> readWholeFile(const Path &path, std::size_t maxBytes) {
    HandleGuard file(CreateFileW(path.native().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        return fileError(lastError(), "open", path);
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) {
        return fileError(lastError(), "size", path);
    }
    if (static_cast<unsigned long long>(size.QuadPart) > maxBytes) {
        return Error(ErrorCode::LimitExceeded, "file too large")
            .with("path", path.toString())
            .with("bytes", std::to_string(size.QuadPart));
    }
    std::vector<std::byte> data(static_cast<std::size_t>(size.QuadPart));
    std::size_t done = 0;
    while (done < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 30));
        DWORD read = 0;
        if (!ReadFile(file.get(), data.data() + done, chunk, &read, nullptr)) {
            return fileError(lastError(), "read", path);
        }
        if (read == 0) {
            data.resize(done); // the file shrank while we read it
            break;
        }
        done += read;
    }
    return data;
}

Result<void> atomicReplace(const Path &path, Span<const std::byte> data) {
    const std::wstring temp = temporarySibling(path);
    HANDLE raw = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        return fileError(lastError(), "create temporary file", path);
    }
    const auto fail = [&](StringView action) -> Result<void> {
        const std::error_code code = lastError();
        CloseHandle(raw);
        DeleteFileW(temp.c_str());
        return fileError(code, action, path);
    };

    std::size_t done = 0;
    while (done < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(raw, data.data() + done, chunk, &written, nullptr) || written == 0) {
            return fail("write");
        }
        done += written;
    }
    if (!FlushFileBuffers(raw)) {
        return fail("flush");
    }
    CloseHandle(raw);

    if (!MoveFileExW(temp.c_str(), path.native().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const std::error_code code = lastError();
        DeleteFileW(temp.c_str());
        return fileError(code, "replace", path);
    }
    return success();
}

Result<Mapping> mapFile(const Path &path, std::size_t maxBytes) {
    HandleGuard file(CreateFileW(path.native().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        return fileError(lastError(), "open", path);
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) {
        return fileError(lastError(), "size", path);
    }
    if (static_cast<unsigned long long>(size.QuadPart) > maxBytes) {
        return Error(ErrorCode::LimitExceeded, "file too large to map").with("path", path.toString());
    }
    Mapping mapping;
    if (size.QuadPart == 0) {
        return mapping; // an empty file maps to an empty span; Windows cannot map zero bytes
    }
    HandleGuard section(CreateFileMappingW(file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (section.get() == nullptr) {
        return fileError(lastError(), "map", path);
    }
    const void *view = MapViewOfFile(section.get(), FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        return fileError(lastError(), "map view", path);
    }
    mapping.data = static_cast<const std::byte *>(view);
    mapping.size = static_cast<std::size_t>(size.QuadPart);
    mapping.fileHandle = file.release();
    mapping.mappingHandle = section.release();
    return mapping;
}

void unmapFile(Mapping &mapping) noexcept {
    if (mapping.data != nullptr) {
        UnmapViewOfFile(mapping.data);
    }
    if (mapping.mappingHandle != nullptr) {
        CloseHandle(mapping.mappingHandle);
    }
    if (mapping.fileHandle != nullptr) {
        CloseHandle(mapping.fileHandle);
    }
    mapping = Mapping{};
}

} // namespace cfw::detail
