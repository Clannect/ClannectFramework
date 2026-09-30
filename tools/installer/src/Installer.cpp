#include "Installer.h"

#include <algorithm>
#include <ctime>
#include <filesystem>

#include "System.h"
#include "cfw/core/Strings.h"
#include "cfw/io/FileSystem.h"
#include "cfw/io/JsonReader.h"
#include "cfw/io/JsonWriter.h"

namespace cfw::installer {

namespace {

constexpr const char *kManifest = "installed.json";
constexpr const char *kSettings = "settings.json";
constexpr const char *kPackagePrefix = "ClannectFramework-";
constexpr const char *kPackageFolderSuffix = "-windows-x64-mingw";

bool isDigits(StringView s) {
    return !s.empty() && s.size() <= 9 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Windows wants backslashes in the commands it runs.
String nativeText(const Path &path) {
    String text = path.toString();
#ifdef _WIN32
    std::replace(text.begin(), text.end(), '/', '\\');
#endif
    return text;
}

std::int64_t now() { return static_cast<std::int64_t>(std::time(nullptr)); }

String uninstallKey(const Version &v) { return "ClannectFramework-" + v.toString(); }

// A folder's size in KB, for the Installed apps list.
std::uint64_t folderBytes(const Path &folder) {
    std::uint64_t total = 0;
    for (const DirectoryEntry &e : listDirectory(folder).valueOr({})) {
        total += e.isDirectory ? folderBytes(e.path) : e.size;
    }
    return total;
}

} // namespace

// ---- Versions ----

std::optional<Version> Version::parse(StringView text) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
        text.remove_prefix(1);
    }
    Version v;
    if (const std::size_t dash = text.find('-'); dash != StringView::npos) {
        v.suffix = String(text.substr(dash + 1));
        text = text.substr(0, dash);
        if (v.suffix.empty()) {
            return std::nullopt;
        }
    }
    const std::vector<StringView> parts = split(text, '.', SplitMode::KeepEmpty);
    if (parts.size() != 3 || !isDigits(parts[0]) || !isDigits(parts[1]) || !isDigits(parts[2])) {
        return std::nullopt;
    }
    v.major = static_cast<int>(parseInt(parts[0]).value());
    v.minor = static_cast<int>(parseInt(parts[1]).value());
    v.patch = static_cast<int>(parseInt(parts[2]).value());
    return v;
}

String Version::toString() const {
    String s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (!suffix.empty()) {
        s += "-" + suffix;
    }
    return s;
}

int compare(const Version &a, const Version &b) {
    for (const auto &[x, y] : {std::pair{a.major, b.major}, std::pair{a.minor, b.minor}, std::pair{a.patch, b.patch}}) {
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    if (a.suffix == b.suffix) {
        return 0;
    }
    if (a.suffix.empty() || b.suffix.empty()) {
        return a.suffix.empty() ? 1 : -1; // 1.0.0-rc.1 < 1.0.0
    }
    // SemVer: dot-separated identifiers, numbers numerically and before words.
    const std::vector<StringView> pa = split(a.suffix, '.', SplitMode::KeepEmpty);
    const std::vector<StringView> pb = split(b.suffix, '.', SplitMode::KeepEmpty);
    for (std::size_t i = 0; i < std::min(pa.size(), pb.size()); ++i) {
        const bool na = isDigits(pa[i]);
        const bool nb = isDigits(pb[i]);
        if (na && nb) {
            const std::int64_t x = parseInt(pa[i]).value();
            const std::int64_t y = parseInt(pb[i]).value();
            if (x != y) {
                return x < y ? -1 : 1;
            }
        } else if (na != nb) {
            return na ? -1 : 1;
        } else if (pa[i] != pb[i]) {
            return pa[i] < pb[i] ? -1 : 1;
        }
    }
    return pa.size() == pb.size() ? 0 : (pa.size() < pb.size() ? -1 : 1);
}

// ---- Releases ----

Result<std::vector<Release>> parseReleases(StringView json) {
    Result<JsonValue> parsed = parseJson(json);
    if (!parsed) {
        return parsed.error();
    }
    const JsonArray *list = parsed.value().asArray();
    if (!list) {
        // GitHub answers errors (rate limits) with an object and a message.
        const String message(parsed.value()["message"].toString("not a list of releases"));
        return Error(ErrorCode::ParseError, "GitHub: " + message);
    }
    std::vector<Release> releases;
    for (const JsonValue &item : *list) {
        if (item["draft"].toBool(false)) {
            continue;
        }
        Release r;
        r.tag = String(item["tag_name"].toString(""));
        const std::optional<Version> version = Version::parse(r.tag);
        if (!version) {
            continue;
        }
        r.version = *version;
        r.title = String(item["name"].toString(r.tag));
        r.prerelease = item["prerelease"].toBool(false);
        r.publishedAt = String(item["published_at"].toString(""));
        r.pageUrl = String(item["html_url"].toString(""));
        if (const JsonArray *assets = item["assets"].asArray()) {
            for (const JsonValue &a : *assets) {
                const StringView name = a["name"].toString("");
                if (!StringView(name).ends_with(kWindowsPackageSuffix)) {
                    continue;
                }
                Asset asset;
                asset.name = String(name);
                asset.url = String(a["browser_download_url"].toString(""));
                asset.size = static_cast<std::uint64_t>(a["size"].toDouble(0));
                if (const StringView digest = a["digest"].toString(""); StringView(digest).starts_with("sha256:")) {
                    asset.sha256 = toLowerAscii(digest.substr(7));
                }
                if (StringView(asset.url).starts_with("https://")) {
                    r.windowsPackage = std::move(asset);
                }
            }
        }
        releases.push_back(std::move(r));
    }
    std::stable_sort(releases.begin(), releases.end(),
                     [](const Release &a, const Release &b) { return compare(b.version, a.version) < 0; });
    return releases;
}

// ---- Packages ----

Result<Package> openPackage(Span<const std::byte> zip) {
    Result<ZipArchive> archive = ZipArchive::open(zip);
    if (!archive) {
        return archive.error();
    }
    const auto notOurs = [](const String &why) {
        return Error(ErrorCode::Corrupt, "not a Clannect Framework package: " + why);
    };
    if (archive.value().entries().empty()) {
        return notOurs("it is empty");
    }
    const String &first = archive.value().entries().front().name;
    const String top = first.substr(0, first.find('/'));
    if (!StringView(top).starts_with(kPackagePrefix) || !StringView(top).ends_with(kPackageFolderSuffix)) {
        return notOurs("its folder is " + top);
    }
    const std::optional<Version> version = Version::parse(
        StringView(top).substr(std::char_traits<char>::length(kPackagePrefix),
                               top.size() - std::char_traits<char>::length(kPackagePrefix) -
                                   std::char_traits<char>::length(kPackageFolderSuffix)));
    if (!version) {
        return notOurs("no version in " + top);
    }
    for (const ZipEntry &e : archive.value().entries()) {
        if (!StringView(e.name).starts_with(top + "/")) {
            return notOurs("an entry outside its folder: " + e.name);
        }
    }
    if (!archive.value().find(top + "/lib/cmake/ClannectFramework/ClannectFrameworkConfig.cmake")) {
        return notOurs("no CMake package inside");
    }
    Package package{*version, std::move(archive.value()), 0};
    package.prefixSize = static_cast<std::size_t>(package.archive.bytes().data() - zip.data());
    return package;
}

Result<std::optional<Package>> findPackage(Span<const std::byte> self) {
    if (!ZipArchive::open(self)) {
        return std::optional<Package>{};
    }
    Result<Package> package = openPackage(self);
    if (!package) {
        return package.error();
    }
    return std::optional<Package>(std::move(package.value()));
}

// ---- What is installed ----

Path versionFolder(const Path &root, const Version &version) { return root / version.toString(); }

Path maintenanceTool(const Path &root) { return root / kMaintenanceFolder / kMaintenanceTool; }

std::vector<Installed> installedVersions(const Path &root) {
    std::vector<Installed> out;
    for (const DirectoryEntry &e : listDirectory(root).valueOr({})) {
        if (!e.isDirectory) {
            continue;
        }
        const Result<JsonValue> manifest = parseJson(readTextFile(e.path / kManifest).valueOr(""));
        if (!manifest) {
            continue;
        }
        const std::optional<Version> version = Version::parse(manifest.value()["version"].toString(""));
        if (!version) {
            continue;
        }
        out.push_back({*version, String(manifest.value()["tag"].toString("")),
                       manifest.value()["prerelease"].toBool(version->isPrerelease()), e.path});
    }
    std::sort(out.begin(), out.end(), [](const Installed &a, const Installed &b) { return compare(b.version, a.version) < 0; });
    return out;
}

Preferences loadPreferences(const Path &root) {
    Preferences p;
    const Result<JsonValue> json = parseJson(readTextFile(root / kMaintenanceFolder / kSettings).valueOr(""));
    if (json) {
        const JsonValue &v = json.value();
        p.notifyUpdates = v["notifyUpdates"].toBool(p.notifyUpdates);
        p.includePrereleases = v["includePrereleases"].toBool(p.includePrereleases);
        p.lastUpdateCheck = v["lastUpdateCheck"].toInteger().value_or(0);
        p.skippedTag = String(v["skippedTag"].toString(""));
    }
    return p;
}

Result<void> savePreferences(const Path &root, const Preferences &p) {
    const JsonObject json{{"notifyUpdates", p.notifyUpdates},
                          {"includePrereleases", p.includePrereleases},
                          {"lastUpdateCheck", p.lastUpdateCheck},
                          {"skippedTag", p.skippedTag}};
    if (Result<void> made = createDirectories(root / kMaintenanceFolder); !made) {
        return made;
    }
    return writeFileAtomic(root / kMaintenanceFolder / kSettings, writeJson(json));
}

String quoteArgument(StringView argument) {
    // Windows' rules (CommandLineToArgvW): backslashes before a quote are
    // doubled; a quote is escaped.
    String out = "\"";
    std::size_t backslashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            backslashes = 0;
        }
        if (c != '\\') {
            out += c;
        }
    }
    out.append(backslashes * 2, '\\');
    return out + "\"";
}

static String logonCommand(const Path &root) {
    return quoteArgument(nativeText(maintenanceTool(root))) + " --check-for-updates";
}

// ---- Installing ----

Result<Path> install(const InstallRequest &request) {
    if (request.package == nullptr) {
        return Error(ErrorCode::InvalidArgument, "nothing to install");
    }
    const auto progress = [&](double fraction, StringView what) {
        return !request.progress || request.progress(fraction, what);
    };
    const Version &version = request.package->version;
    const Path folder = versionFolder(request.root, version);
    if (isDirectory(folder)) {
        if (!isFile(folder / kManifest) && !listDirectory(folder).valueOr({}).empty()) {
            return Error(ErrorCode::AlreadyExists,
                         folder.toString() + " already exists and is not a Clannect Framework installation");
        }
    }

    // Extract next to the final folder, then swap it in: an interrupted
    // install never leaves half a version where a whole one was.
    const Path partial = request.root / (version.toString() + ".partial");
    (void)removeAll(partial);
    ExtractOptions options;
    options.stripComponents = 1;
    options.progress = [&](std::uint64_t done, std::uint64_t total) {
        return progress(total ? 0.9 * static_cast<double>(done) / static_cast<double>(total) : 0.9, "Extracting");
    };
    if (Result<void> extracted = extractZip(request.package->archive, partial, options); !extracted) {
        (void)removeAll(partial);
        return extracted.error();
    }
    if (isDirectory(folder)) {
        if (Result<void> removed = removeAll(folder); !removed) {
            (void)removeAll(partial);
            return Error(removed.error().code(), "could not replace " + folder.toString() + ": " +
                                                     removed.error().message() + " (is it in use?)");
        }
    }
    if (Result<void> moved = renameFile(partial, folder); !moved) {
        (void)removeAll(partial);
        return moved.error();
    }
    const JsonObject manifest{{"version", version.toString()},
                              {"tag", request.tag.empty() ? "v" + version.toString() : request.tag},
                              {"prerelease", version.isPrerelease() || request.prerelease},
                              {"installedAt", now()}};
    if (Result<void> written = writeFileAtomic(folder / kManifest, writeJson(manifest)); !written) {
        return written.error();
    }
    if (!progress(0.92, "Installing the maintenance tool")) {
        return Error(ErrorCode::Cancelled, "cancelled");
    }
    if (!request.maintenanceTool.empty()) {
        if (Result<void> made = createDirectories(request.root / kMaintenanceFolder); !made) {
            return made.error();
        }
        if (Result<void> written = writeFileAtomic(maintenanceTool(request.root), request.maintenanceTool); !written) {
            // A running maintenance tool cannot be replaced; the old one works.
            if (!isFile(maintenanceTool(request.root))) {
                return written.error();
            }
        }
#ifndef _WIN32
        std::error_code ignored;
        std::filesystem::permissions(maintenanceTool(request.root).native(), std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::add, ignored);
#endif
    }
    Preferences prefs = loadPreferences(request.root);
    if (request.notifyUpdates) {
        prefs.notifyUpdates = *request.notifyUpdates;
        prefs.includePrereleases = request.includePrereleases;
        if (Result<void> saved = savePreferences(request.root, prefs); !saved) {
            return saved.error();
        }
    }

    if (!progress(0.95, "Registering with Windows")) {
        return Error(ErrorCode::Cancelled, "cancelled");
    }
    UninstallEntry entry;
    entry.key = uninstallKey(version);
    entry.displayName = "Clannect Framework " + version.toString();
    entry.displayVersion = version.toString();
    entry.publisher = "Clannect";
    entry.installLocation = folder;
    entry.uninstallCommand =
        quoteArgument(nativeText(maintenanceTool(request.root))) + " --uninstall " + quoteArgument(nativeText(folder));
    entry.helpLink = kProjectUrl;
    entry.estimatedSizeKb = static_cast<std::uint32_t>(std::min<std::uint64_t>(folderBytes(folder) / 1024, 0xFFFFFFFFu));
    if (Result<void> registered = registerUninstallEntry(entry); !registered) {
        return registered.error();
    }
    if (request.registerWithCMake) {
        if (Result<void> registered = registerCMakePackage(uninstallKey(version), folder / "lib/cmake/ClannectFramework");
            !registered) {
            return registered.error();
        }
    }
    if (request.notifyUpdates) {
        if (Result<void> set = setLogonCommand(*request.notifyUpdates ? std::optional<String>(logonCommand(request.root))
                                                                       : std::nullopt);
            !set) {
            return set.error();
        }
    }
    progress(1.0, "Done");
    return folder;
}

Result<void> uninstall(const Path &folder) {
    const Result<JsonValue> manifest = parseJson(readTextFile(folder / kManifest).valueOr(""));
    const std::optional<Version> version =
        manifest ? Version::parse(manifest.value()["version"].toString("")) : std::nullopt;
    if (!version) {
        return Error(ErrorCode::NotFound, folder.toString() + " is not a Clannect Framework installation");
    }
    (void)removeUninstallEntry(uninstallKey(*version));
    (void)removeCMakePackage(uninstallKey(*version));
    if (Result<void> removed = removeAll(folder); !removed) {
        return Error(removed.error().code(), "could not remove " + folder.toString() + ": " +
                                                 removed.error().message() + " (is it in use?)");
    }
    const Path root = folder.parent();
    if (installedVersions(root).empty()) {
        (void)setLogonCommand(std::nullopt);
        // The maintenance tool may be the program running this, which
        // Windows will not delete; main.cpp then removes it on exit.
        (void)removeAll(root / kMaintenanceFolder / kSettings);
        (void)removeAll(root / kMaintenanceFolder);
        if (listDirectory(root).valueOr({}).empty()) {
            (void)removeAll(root);
        }
    }
    return {};
}

std::optional<Release> newerRelease(const std::vector<Release> &releases, const std::vector<Installed> &installed,
                                    const Preferences &prefs) {
    if (installed.empty()) {
        return std::nullopt;
    }
    const Version &newest = installed.front().version; // newest first
    for (const Release &r : releases) {
        if (!r.windowsPackage || (r.prerelease && !prefs.includePrereleases)) {
            continue;
        }
        if (compare(r.version, newest) <= 0) {
            return std::nullopt; // sorted: nothing newer follows
        }
        if (r.tag == prefs.skippedTag) {
            continue;
        }
        return r;
    }
    return std::nullopt;
}

} // namespace cfw::installer
