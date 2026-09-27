// POSIX (Linux, macOS) implementation of PlatformFile.h.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <random>

#include "PlatformFile.h"

namespace cfw::detail {

namespace {

std::error_code lastError() noexcept { return std::error_code(errno, std::generic_category()); }

class FdGuard {
public:
    explicit FdGuard(int fd) noexcept : m_fd(fd) {}
    ~FdGuard() {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
    }
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;
    [[nodiscard]] int get() const noexcept { return m_fd; }

private:
    int m_fd;
};

} // namespace

Error fileError(const std::error_code &code, StringView action, const Path &path) {
    ErrorCode ce = ErrorCode::IoError;
    if (code == std::errc::no_such_file_or_directory || code == std::errc::not_a_directory) {
        ce = ErrorCode::NotFound;
    } else if (code == std::errc::permission_denied || code == std::errc::operation_not_permitted ||
               code == std::errc::read_only_file_system) {
        ce = ErrorCode::PermissionDenied;
    } else if (code == std::errc::file_exists) {
        ce = ErrorCode::AlreadyExists;
    } else if (code == std::errc::not_enough_memory) {
        ce = ErrorCode::OutOfMemory;
    }
    String message(action);
    message += " failed: ";
    message += code.message();
    return Error(ce, std::move(message)).with("path", path.toString()).with("os-error", std::to_string(code.value()));
}

Result<std::vector<std::byte>> readWholeFile(const Path &path, std::size_t maxBytes) {
    FdGuard fd(::open(path.native().c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0) {
        return fileError(lastError(), "open", path);
    }
    struct stat info {};
    if (::fstat(fd.get(), &info) != 0) {
        return fileError(lastError(), "size", path);
    }
    if (static_cast<unsigned long long>(info.st_size) > maxBytes) {
        return Error(ErrorCode::LimitExceeded, "file too large").with("path", path.toString());
    }
    std::vector<std::byte> data(static_cast<std::size_t>(info.st_size));
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::read(fd.get(), data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fileError(lastError(), "read", path);
        }
        if (n == 0) {
            data.resize(done);
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    return data;
}

Result<void> atomicReplace(const Path &path, Span<const std::byte> data) {
    std::random_device entropy;
    char suffix[20];
    std::snprintf(suffix, sizeof suffix, ".cfw-tmp-%08x", entropy());
    const std::string temp = path.native().string() + suffix;

    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        return fileError(lastError(), "create temporary file", path);
    }
    const auto fail = [&](StringView action) -> Result<void> {
        const std::error_code code = lastError();
        ::close(fd);
        ::unlink(temp.c_str());
        return fileError(code, action, path);
    };
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail("write");
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        return fail("flush");
    }
    ::close(fd);
    if (::rename(temp.c_str(), path.native().c_str()) != 0) {
        const std::error_code code = lastError();
        ::unlink(temp.c_str());
        return fileError(code, "replace", path);
    }
    // Make the rename itself durable.
    const std::string dir = path.native().parent_path().empty() ? "." : path.native().parent_path().string();
    FdGuard dirFd(::open(dir.c_str(), O_RDONLY | O_CLOEXEC));
    if (dirFd.get() >= 0) {
        (void)::fsync(dirFd.get());
    }
    return success();
}

Result<Mapping> mapFile(const Path &path, std::size_t maxBytes) {
    FdGuard fd(::open(path.native().c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0) {
        return fileError(lastError(), "open", path);
    }
    struct stat info {};
    if (::fstat(fd.get(), &info) != 0) {
        return fileError(lastError(), "size", path);
    }
    if (static_cast<unsigned long long>(info.st_size) > maxBytes) {
        return Error(ErrorCode::LimitExceeded, "file too large to map").with("path", path.toString());
    }
    Mapping mapping;
    if (info.st_size == 0) {
        return mapping;
    }
    void *view = ::mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_PRIVATE, fd.get(), 0);
    if (view == MAP_FAILED) {
        return fileError(lastError(), "map", path);
    }
    mapping.data = static_cast<const std::byte *>(view);
    mapping.size = static_cast<std::size_t>(info.st_size);
    return mapping; // the mapping stays valid after the descriptor closes
}

void unmapFile(Mapping &mapping) noexcept {
    if (mapping.data != nullptr) {
        ::munmap(const_cast<std::byte *>(mapping.data), mapping.size);
    }
    mapping = Mapping{};
}

} // namespace cfw::detail
