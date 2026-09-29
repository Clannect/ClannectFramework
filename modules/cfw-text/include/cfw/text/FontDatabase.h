#pragma once

// The fonts an application can use: files and data it registers, and the
// system's installed fonts. Finds a face by family, weight and style (the
// nearest, as CSS matching does), and a fallback face for a character the
// primary font lacks.
//
// Faces are loaded lazily: registering a file reads its names and styles;
// the face itself (and its data) stays loaded once used. System directories:
// %WINDIR%\Fonts and the user font folder on Windows, /System/Library/Fonts,
// /Library/Fonts and ~/Library/Fonts on macOS, and the XDG font directories
// (/usr/share/fonts, /usr/local/share/fonts, ~/.local/share/fonts, ~/.fonts)
// elsewhere.
//
// Threads: one database per thread, or external locking.

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/String.h"
#include "cfw/io/Path.h"
#include "cfw/text/FontFace.h"

namespace cfw {

class FontDatabase {
public:
    struct FaceInfo {
        String family;
        String style;
        int weight = 400;
        bool italic = false;
        Path file;               // empty for data registered in memory
        std::uint32_t index = 0; // face index in a collection
    };

    // Registers every face of a font file (or of font data). Returns the
    // number of faces added.
    Result<std::size_t> addFile(const Path &file);
    Result<std::size_t> addData(FontFace::Data data);
    // Registers the installed fonts; returns how many faces were added. The
    // font folders are listed on the first call in the process only: later
    // calls (from any database) reuse that list.
    std::size_t addSystemFonts();
    // Registers the fonts under a directory (recursively).
    std::size_t addDirectory(const Path &directory);

    [[nodiscard]] const std::vector<FaceInfo> &faces() const noexcept { return m_faces; }
    [[nodiscard]] std::vector<String> families() const;

    // The face (loaded on first use); null if it fails to load.
    [[nodiscard]] std::shared_ptr<const FontFace> face(std::size_t index);

    // The face of `family` (case-insensitive) nearest to `weight` and
    // `italic`; null if no face has the family.
    [[nodiscard]] std::shared_ptr<const FontFace> match(StringView family, int weight = 400, bool italic = false);

    // A face with a glyph for `c`, preferring the style of `like` (its own
    // family first, then the nearest weight and slant); null if none has
    // one. Answers are cached per character.
    [[nodiscard]] std::shared_ptr<const FontFace> fallback(char32_t c, const FontFace *like = nullptr);

private:
    std::size_t addFaces(const FontFace::Data &data, const Path &file);
    std::size_t addSystemFontsUncached();
    [[nodiscard]] int distance(const FaceInfo &info, int weight, bool italic) const;

    std::vector<FaceInfo> m_faces;
    std::vector<std::shared_ptr<const FontFace>> m_loaded; // per face, when loaded
    std::vector<FontFace::Data> m_data;                     // per face, when registered from memory
    std::unordered_map<std::uint64_t, int> m_fallback;      // (style key, character) -> face index or -1
};

} // namespace cfw
