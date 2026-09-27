#pragma once

#include "cfw/core/Result.h"
#include "cfw/io/Path.h"

namespace cfw {

// A new, uniquely named directory under the system temporary directory,
// removed with everything in it when this object is destroyed. For tests,
// import staging and scratch work.
//
// Threads: one owner. Allocates: the path.
class TemporaryDirectory {
public:
    // `prefix` becomes the start of the directory's name ("cfw-test-3fa2...").
    [[nodiscard]] static Result<TemporaryDirectory> create(StringView prefix = "cfw-");

    TemporaryDirectory(TemporaryDirectory &&other) noexcept;
    TemporaryDirectory &operator=(TemporaryDirectory &&other) noexcept;
    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
    ~TemporaryDirectory();

    [[nodiscard]] const Path &path() const noexcept { return m_path; }
    // Keeps the directory on disk after destruction (e.g. to inspect a failed test).
    void keep() noexcept { m_keep = true; }

private:
    explicit TemporaryDirectory(Path path) : m_path(std::move(path)) {}

    Path m_path;
    bool m_keep = false;
};

} // namespace cfw
