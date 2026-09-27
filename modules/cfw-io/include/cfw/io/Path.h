#pragma once

#include <compare>
#include <filesystem>
#include <ostream>

#include "cfw/core/String.h"

namespace cfw {

// A file system path with a UTF-8 interface. Internally a
// std::filesystem::path, so the platform encoding (UTF-16 on Windows) is only
// ever produced at the OS boundary.
//
// toString() always uses '/' separators, on every platform, so paths written
// into project files and logs are the same everywhere; native() gives the
// platform form for OS calls.
//
// Threads: a value type. Allocates: the path text.
class Path {
public:
    Path() = default;
    // From UTF-8 text. Either separator is accepted on Windows.
    Path(StringView utf8);
    Path(const char *utf8) : Path(StringView(utf8)) {}
    Path(const String &utf8) : Path(StringView(utf8)) {}
    explicit Path(std::filesystem::path native) : m_path(std::move(native)) {}

    [[nodiscard]] String toString() const;
    [[nodiscard]] const std::filesystem::path &native() const noexcept { return m_path; }

    [[nodiscard]] bool empty() const noexcept { return m_path.empty(); }
    [[nodiscard]] bool isAbsolute() const { return m_path.is_absolute(); }
    [[nodiscard]] Path parent() const { return Path(m_path.parent_path()); }
    // "scene.cescene"
    [[nodiscard]] String fileName() const;
    // "scene"
    [[nodiscard]] String stem() const;
    // ".cescene" (with the dot), or empty.
    [[nodiscard]] String extension() const;
    [[nodiscard]] Path withExtension(StringView extension) const;
    // Lexically normalised: "a/./b/../c" -> "a/c". Does not touch the disk.
    [[nodiscard]] Path normalized() const { return Path(m_path.lexically_normal()); }

    // Joins; an absolute `child` replaces this path, as in std::filesystem.
    // Text on the right converts implicitly: path / "assets" / name.
    friend Path operator/(const Path &base, const Path &child) { return Path(base.m_path / child.m_path); }

    friend bool operator==(const Path &a, const Path &b) { return a.m_path == b.m_path; }
    friend std::strong_ordering operator<=>(const Path &a, const Path &b) {
        return a.m_path.compare(b.m_path) <=> 0;
    }
    friend std::ostream &operator<<(std::ostream &out, const Path &path) { return out << path.toString(); }

private:
    std::filesystem::path m_path;
};

} // namespace cfw
