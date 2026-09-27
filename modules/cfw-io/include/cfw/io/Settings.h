#pragma once

#include <cstdint>

#include "cfw/core/Result.h"
#include "cfw/core/String.h"
#include "cfw/io/Json.h"
#include "cfw/io/Path.h"

namespace cfw {

// A small typed key-value store persisted as a JSON file: window layout,
// recent projects, preferences. Replaces QSettings.
//
// Keys are flat strings; use "/" to group ("editor/layout", "figma/lastFile").
// Getters take the fallback returned when the key is missing or holds the
// wrong type, so a hand-edited or older settings file can never break a
// caller. save() writes atomically and only if something changed.
//
// Not for secrets: the file is plain text. Tokens belong in the operating
// system's credential store (cfw-platform).
//
// Threads: one thread at a time. Allocates: the stored values.
class Settings {
public:
    // Loads `file`. A missing file gives empty settings (first run). A file
    // that is not a JSON object is an error; the caller decides whether to
    // start fresh (and whether to keep the broken file aside).
    [[nodiscard]] static Result<Settings> open(Path file);
    // Settings that are never saved (tests, --no-settings).
    [[nodiscard]] static Settings inMemory();

    [[nodiscard]] bool getBool(StringView key, bool fallback) const noexcept;
    [[nodiscard]] std::int64_t getInt(StringView key, std::int64_t fallback) const noexcept;
    [[nodiscard]] double getDouble(StringView key, double fallback) const noexcept;
    [[nodiscard]] String getString(StringView key, StringView fallback) const;
    // Any stored value, or null.
    [[nodiscard]] const JsonValue *get(StringView key) const noexcept { return m_values.find(key); }
    [[nodiscard]] bool contains(StringView key) const noexcept { return m_values.contains(key); }

    void set(StringView key, JsonValue value);
    bool remove(StringView key);

    [[nodiscard]] bool isDirty() const noexcept { return m_dirty; }
    [[nodiscard]] const Path &path() const noexcept { return m_path; }
    // Writes the file if anything changed since the last load or save. An
    // in-memory store succeeds without writing.
    [[nodiscard]] Result<void> save();

private:
    Settings() = default;

    Path m_path;
    JsonObject m_values;
    bool m_dirty = false;
    bool m_persistent = false;
};

} // namespace cfw
