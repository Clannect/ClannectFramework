#include "cfw/text/FontDatabase.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <numeric>

#include "cfw/io/FileSystem.h"

namespace cfw {

namespace {

bool equalsIgnoreCase(StringView a, StringView b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool isFontFile(const Path &p) {
    String ext = p.extension();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc";
}

Path fromEnvironment(const char *name) {
    const char *value = std::getenv(name);
    return value != nullptr && *value != 0 ? Path(value) : Path();
}

} // namespace

std::size_t FontDatabase::addFaces(const FontFace::Data &data, const Path &file) {
    const Result<std::uint32_t> count = FontFace::faceCount(*data);
    if (!count) {
        return 0;
    }
    std::size_t added = 0;
    for (std::uint32_t i = 0; i < count.value() && i < 256; ++i) {
        const Result<std::shared_ptr<const FontFace>> face = FontFace::load(data, i);
        if (!face || face.value()->familyName().empty()) {
            continue;
        }
        const FontFace &f = *face.value();
        m_faces.push_back({f.familyName(), f.styleName(), f.weight(), f.isItalic(), file, i});
        // Faces from files are loaded again on use, so their data is not kept.
        m_loaded.push_back(file.empty() ? face.value() : nullptr);
        m_data.push_back(file.empty() ? data : nullptr);
        ++added;
    }
    if (added) {
        m_fallback.clear();
    }
    return added;
}

Result<std::size_t> FontDatabase::addFile(const Path &file) {
    Result<std::vector<std::byte>> bytes = readFile(file, 256u << 20);
    if (!bytes) {
        return bytes.error();
    }
    return addFaces(std::make_shared<const std::vector<std::byte>>(std::move(bytes.value())), file);
}

Result<std::size_t> FontDatabase::addData(FontFace::Data data) {
    if (!data) {
        return Error(ErrorCode::InvalidArgument, "no font data");
    }
    return addFaces(data, Path());
}

std::size_t FontDatabase::addDirectory(const Path &directory) {
    std::size_t added = 0;
    std::vector<Path> pending{directory};
    std::size_t visited = 0;
    while (!pending.empty() && visited < 4096) {
        const Path dir = pending.back();
        pending.pop_back();
        ++visited;
        const Result<std::vector<DirectoryEntry>> entries = listDirectory(dir);
        if (!entries) {
            continue;
        }
        for (const DirectoryEntry &e : entries.value()) {
            if (e.isDirectory) {
                pending.push_back(e.path);
            } else if (isFontFile(e.path)) {
                added += addFile(e.path).valueOr(0);
            }
        }
    }
    return added;
}

std::size_t FontDatabase::addSystemFonts() {
    std::vector<Path> dirs;
#if defined(_WIN32)
    if (const Path windir = fromEnvironment("WINDIR"); !windir.empty()) {
        dirs.push_back(windir / "Fonts");
    }
    if (const Path local = fromEnvironment("LOCALAPPDATA"); !local.empty()) {
        dirs.push_back(local / "Microsoft" / "Windows" / "Fonts");
    }
#elif defined(__APPLE__)
    dirs = {Path("/System/Library/Fonts"), Path("/Library/Fonts")};
    if (const Path home = fromEnvironment("HOME"); !home.empty()) {
        dirs.push_back(home / "Library" / "Fonts");
    }
#else
    dirs = {Path("/usr/share/fonts"), Path("/usr/local/share/fonts")};
    if (const Path data = fromEnvironment("XDG_DATA_HOME"); !data.empty()) {
        dirs.push_back(data / "fonts");
    }
    if (const Path home = fromEnvironment("HOME"); !home.empty()) {
        dirs.push_back(home / ".local" / "share" / "fonts");
        dirs.push_back(home / ".fonts");
    }
#endif
    std::size_t added = 0;
    for (const Path &d : dirs) {
        if (isDirectory(d)) {
            added += addDirectory(d);
        }
    }
    return added;
}

std::vector<String> FontDatabase::families() const {
    std::vector<String> out;
    for (const FaceInfo &f : m_faces) {
        if (std::none_of(out.begin(), out.end(), [&](const String &s) { return equalsIgnoreCase(s, f.family); })) {
            out.push_back(f.family);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::shared_ptr<const FontFace> FontDatabase::face(std::size_t index) {
    if (index >= m_faces.size()) {
        return nullptr;
    }
    if (!m_loaded[index] && !m_faces[index].file.empty()) {
        Result<std::vector<std::byte>> bytes = readFile(m_faces[index].file, 256u << 20);
        if (bytes) {
            auto data = std::make_shared<const std::vector<std::byte>>(std::move(bytes.value()));
            if (Result<std::shared_ptr<const FontFace>> f = FontFace::load(data, m_faces[index].index)) {
                m_loaded[index] = f.value();
                // Other faces of the same collection share the data.
                for (std::size_t j = 0; j < m_faces.size(); ++j) {
                    if (j != index && !m_loaded[j] && m_faces[j].file == m_faces[index].file) {
                        if (Result<std::shared_ptr<const FontFace>> g = FontFace::load(data, m_faces[j].index)) {
                            m_loaded[j] = g.value();
                        }
                    }
                }
            }
        }
    }
    return m_loaded[index];
}

int FontDatabase::distance(const FaceInfo &info, int weight, bool italic) const {
    int d = std::abs(info.weight - weight);
    // CSS: for weights up to 500 lighter faces are preferred, above it heavier ones.
    if ((weight <= 500 && info.weight > weight) || (weight > 500 && info.weight < weight)) {
        d += 1000;
    }
    return d + (info.italic != italic ? 10000 : 0);
}

std::shared_ptr<const FontFace> FontDatabase::match(StringView family, int weight, bool italic) {
    int best = -1;
    int bestDistance = 0;
    for (std::size_t i = 0; i < m_faces.size(); ++i) {
        if (!equalsIgnoreCase(m_faces[i].family, family)) {
            continue;
        }
        const int d = distance(m_faces[i], weight, italic);
        if (best < 0 || d < bestDistance) {
            best = static_cast<int>(i);
            bestDistance = d;
        }
    }
    return best < 0 ? nullptr : face(static_cast<std::size_t>(best));
}

std::shared_ptr<const FontFace> FontDatabase::fallback(char32_t c, const FontFace *like) {
    const int weight = like ? like->weight() : 400;
    const bool italic = like ? like->isItalic() : false;
    const String family = like ? like->familyName() : String();
    const std::uint64_t key = (static_cast<std::uint64_t>(weight) << 1 | (italic ? 1u : 0u)) << 32 | c;
    if (const auto it = m_fallback.find(key); it != m_fallback.end()) {
        return it->second < 0 ? nullptr : face(static_cast<std::size_t>(it->second));
    }
    // Candidates: the same family first, then by style distance, then in
    // registration order.
    std::vector<std::size_t> order(m_faces.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const bool fa = equalsIgnoreCase(m_faces[a].family, family);
        const bool fb = equalsIgnoreCase(m_faces[b].family, family);
        if (fa != fb) {
            return fa;
        }
        return distance(m_faces[a], weight, italic) < distance(m_faces[b], weight, italic);
    });
    int found = -1;
    for (const std::size_t i : order) {
        const std::shared_ptr<const FontFace> f = face(i);
        if (f && f.get() != like && f->glyphIndex(c) != 0) {
            found = static_cast<int>(i);
            break;
        }
    }
    m_fallback.emplace(key, found);
    return found < 0 ? nullptr : face(static_cast<std::size_t>(found));
}

} // namespace cfw
