#include "cfw/io/FileWatcher.h"

#include <algorithm>

#include "cfw/io/FileSystem.h"

namespace cfw {

FileWatcher::Snapshot FileWatcher::scan(const Path &path) {
    Snapshot snapshot;
    const auto stampOf = [](const Path &file) -> Stamp {
        return {fileSize(file).valueOr(0), lastWriteTime(file).valueOr(0)};
    };
    if (isDirectory(path)) {
        if (Result<std::vector<DirectoryEntry>> entries = listDirectory(path)) {
            for (const DirectoryEntry &entry : entries.value()) {
                if (!entry.isDirectory) {
                    snapshot.insertOrAssign(entry.path, stampOf(entry.path));
                }
            }
        }
    } else if (exists(path)) {
        snapshot.insertOrAssign(path, stampOf(path));
    }
    return snapshot;
}

void FileWatcher::watch(const Path &path) {
    if (!m_watched.contains(path)) {
        m_watched.insertOrAssign(path, scan(path));
    }
}

void FileWatcher::unwatch(const Path &path) { m_watched.erase(path); }

std::vector<FileChange> FileWatcher::poll() {
    std::vector<FileChange> changes;
    for (auto &[root, previous] : m_watched) {
        Snapshot current = scan(root);
        for (const auto &[file, stamp] : current) {
            const Stamp *before = previous.get(file);
            if (before == nullptr) {
                changes.push_back({file, FileChange::Kind::Created});
            } else if (before->size != stamp.size || before->modified != stamp.modified) {
                changes.push_back({file, FileChange::Kind::Modified});
            }
        }
        for (const auto &[file, stamp] : previous) {
            if (!current.contains(file)) {
                changes.push_back({file, FileChange::Kind::Removed});
            }
        }
        previous = std::move(current);
    }
    std::sort(changes.begin(), changes.end(), [](const FileChange &a, const FileChange &b) { return a.path < b.path; });
    return changes;
}

} // namespace cfw
