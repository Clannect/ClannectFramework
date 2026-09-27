#pragma once

#include <cstdint>
#include <vector>

#include "cfw/core/FlatMap.h"
#include "cfw/io/Path.h"

namespace cfw {

struct FileChange {
    enum class Kind : std::uint8_t { Created, Modified, Removed };
    Path path;
    Kind kind = Kind::Modified;
};

// Reports changes to watched files and to the direct children of watched
// directories (asset hot-reload, "the scene changed on disk").
//
// This implementation polls: poll() compares each watched entry's existence,
// size and modification time with the previous poll. The frame loop calls it
// about once a second. An OS-notified implementation (ReadDirectoryChangesW,
// inotify, FSEvents) needs cfw-net's event loop and can replace it behind the
// same interface in M2; see docs/decisions/0009.
//
// Threads: one thread at a time. Allocates: one snapshot entry per watched file.
class FileWatcher {
public:
    // Watching a directory watches its direct children (not recursively).
    // Watching the same path twice is harmless.
    void watch(const Path &path);
    void unwatch(const Path &path);

    // Changes since the previous poll (or since watch()), sorted by path.
    [[nodiscard]] std::vector<FileChange> poll();

private:
    struct Stamp {
        std::uint64_t size = 0;
        std::int64_t modified = 0;
    };
    using Snapshot = FlatMap<Path, Stamp>;

    [[nodiscard]] static Snapshot scan(const Path &path);

    FlatMap<Path, Snapshot> m_watched;
};

} // namespace cfw
