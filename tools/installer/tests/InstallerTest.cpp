// The installer's logic: versions, GitHub's releases list, packages inside
// an offline installer, installing and uninstalling (with the registry
// entries on Windows, where this runs under Wine in CI), the update
// decision, and downloads from a local server (redirects, checksums,
// truncation).

#include "Download.h"
#include "Installer.h"
#include "System.h"

#include <map>

#include "cfw/core/Sha256.h"
#include "cfw/core/Strings.h"
#include "cfw/io/FileSystem.h"
#include "cfw/io/TemporaryDirectory.h"
#include "cfw/net/EventLoop.h"
#include "cfw/net/Executor.h"
#include "cfw/net/HttpBodyDecoder.h"
#include "cfw/net/TcpListener.h"
#include "cfw/test/Check.h"

using namespace cfw;
using namespace cfw::installer;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

const Path kData(CFW_INSTALLER_TESTDATA);

std::vector<std::byte> load(StringView name) { return readFile(kData / name).valueOr({}); }

Version v(StringView text) { return Version::parse(text).value(); }

void versions() {
    check(Version::parse("0.1.2") == Version{0, 1, 2, ""}, "a plain version");
    check(Version::parse("v1.10.0-rc.1") == Version{1, 10, 0, "rc.1"}, "a tag with a pre-release suffix");
    for (const char *bad : {"", "v", "1", "1.2", "1.2.3.4", "1.x.3", "nightly", "1.2.3-", "-1.2.3", "01234567890.0.0"}) {
        check(!Version::parse(bad), (String("not a version: ") + bad).c_str());
    }
    checkEqual(v("v1.2.3-beta").toString(), String("1.2.3-beta"), "toString drops the v");
    // SemVer's own example ordering.
    const char *ordered[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
                             "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "1.10.0", "2.0.0"};
    for (std::size_t i = 0; i + 1 < std::size(ordered); ++i) {
        check(compare(v(ordered[i]), v(ordered[i + 1])) < 0 && compare(v(ordered[i + 1]), v(ordered[i])) > 0,
              (String(ordered[i]) + " < " + ordered[i + 1]).c_str());
    }
    check(compare(v("0.1.1"), v("v0.1.1")) == 0, "equal versions");
}

void releases() {
    const Result<std::vector<Release>> list = parseReleases(readTextFile(kData / "releases.json").valueOr(""));
    check(list.ok(), "the releases list parses");
    if (!list) {
        return;
    }
    std::vector<String> tags;
    for (const Release &r : list.value()) {
        tags.push_back(r.tag);
    }
    checkEqual(join(tags, " "), String("v1.1.0-rc.1 v1.0.0 v0.1.1 v0.1.0"),
               "drafts and non-version tags are left out; newest first");
    const Release &r011 = list.value()[2];
    check(r011.prerelease && r011.windowsPackage && r011.windowsPackage->size == 3571086, "the Windows package asset");
    checkEqual(r011.windowsPackage ? r011.windowsPackage->sha256 : String(),
               String("fddfdc1d52ee6ca526bcf0f7e0f27676a91bea72adec4c1ebcb6bfb194c88e54"), "its digest, lower case");
    check(!list.value()[3].windowsPackage, "a package not served over https is ignored");
    check(list.value()[1].windowsPackage && list.value()[1].windowsPackage->sha256.empty(), "a package without a digest");

    const Result<std::vector<Release>> limited =
        parseReleases(R"({"message": "API rate limit exceeded", "documentation_url": "x"})");
    check(!limited && contains(limited.error().message(), "rate limit"), "GitHub's error message comes through");
    check(!parseReleases("not json"), "garbage is an error");

    // What an installed version is told about.
    const std::vector<Installed> have011{{v("0.1.1"), "v0.1.1", true, Path()}};
    Preferences prefs;
    prefs.includePrereleases = true;
    std::optional<Release> newer = newerRelease(list.value(), have011, prefs);
    check(newer && newer->tag == "v1.1.0-rc.1", "the newest, pre-releases included");
    prefs.includePrereleases = false;
    newer = newerRelease(list.value(), have011, prefs);
    check(newer && newer->tag == "v1.0.0", "the newest stable one otherwise");
    prefs.skippedTag = "v1.0.0";
    check(!newerRelease(list.value(), have011, prefs), "a skipped version is not offered");
    prefs = {};
    const std::vector<Installed> have110{{v("1.1.0-rc.1"), "v1.1.0-rc.1", true, Path()}};
    check(!newerRelease(list.value(), have110, prefs), "nothing when the newest is installed");
    check(!newerRelease(list.value(), {}, prefs), "nothing when nothing is installed");
}

void packages() {
    const std::vector<std::byte> zip = load("ClannectFramework-0.9.0-windows-x64-mingw.zip");
    const Result<Package> package = openPackage(zip);
    check(package.ok() && package.value().version == v("0.9.0") && package.value().prefixSize == 0,
          "a release package opens, with its version");

    // An offline installer: the program, then the package.
    std::vector<std::byte> offline(7000, std::byte{0x90});
    offline[0] = std::byte{'M'};
    offline[1] = std::byte{'Z'};
    offline.insert(offline.end(), zip.begin(), zip.end());
    const Result<std::optional<Package>> found = findPackage(offline);
    check(found.ok() && found.value() && found.value()->prefixSize == 7000,
          "an offline installer finds its package, after the program");
    const Result<std::optional<Package>> none = findPackage(Span<const std::byte>(offline.data(), 7000));
    check(none.ok() && !none.value(), "the online installer has none");

    const std::vector<std::byte> wrong = load("wrong-folder.zip");
    check(!openPackage(wrong), "a zip with another folder is not a package");
    const std::vector<std::byte> noCmake = load("no-cmake.zip");
    check(!openPackage(noCmake), "a zip without the CMake package is not one");
    const std::vector<std::byte> rc = load("ClannectFramework-1.0.0-rc.1-windows-x64-mingw.zip");
    check(openPackage(rc).ok() && openPackage(rc).value().version == v("1.0.0-rc.1"), "a pre-release package");
}

void installing() {
    auto temp = TemporaryDirectory::create();
    const Path root = temp.value().path() / "Programs" / "ClannectFramework";
    const std::vector<std::byte> zip = load("ClannectFramework-0.9.0-windows-x64-mingw.zip");
    const Package package = openPackage(zip).value();
    const std::vector<std::byte> tool(3000, std::byte{0x42});

    InstallRequest request;
    request.root = root;
    request.package = &package;
    request.tag = "v0.9.0";
    request.prerelease = true;
    request.maintenanceTool = tool;
    request.notifyUpdates = true;
    double lastFraction = -1;
    request.progress = [&](double fraction, StringView) {
        check(fraction >= lastFraction, "progress only grows");
        lastFraction = fraction;
        return true;
    };
    const Result<Path> installed = install(request);
    check(installed.ok(), installed ? "installs" : installed.error().message().c_str());
    const Path folder = versionFolder(root, v("0.9.0"));
    check(isFile(folder / "lib/cmake/ClannectFramework/ClannectFrameworkConfig.cmake") &&
              isFile(folder / "bin/cfw-ui-gallery.exe"),
          "the package's files, without its top folder");
    check(!exists(folder / "ClannectFramework-0.9.0-windows-x64-mingw"), "no nested folder");
    check(!exists(root / "0.9.0.partial"), "no partial folder left behind");
    check(readFile(maintenanceTool(root)).valueOr({}) == tool, "the maintenance tool is copied in");
    checkEqual(lastFraction, 1.0, "progress ends at 1");
    const std::vector<Installed> list = installedVersions(root);
    check(list.size() == 1 && list[0].version == v("0.9.0") && list[0].tag == "v0.9.0" && list[0].prerelease,
          "installedVersions sees it");
    check(loadPreferences(root).notifyUpdates, "the update preference is saved");

#ifdef _WIN32
    const String key = String(kUninstallKey) + "ClannectFramework-0.9.0";
    checkEqual(readUserRegistryString(key, "DisplayName").value_or(""), String("Clannect Framework 0.9.0"),
               "an Installed apps entry");
    check(contains(readUserRegistryString(key, "UninstallString").value_or(""), "--uninstall"),
          "whose uninstall runs the maintenance tool");
    check(readUserRegistryString(kCMakePackagesKey, "ClannectFramework-0.9.0").has_value(), "registered with CMake");
    check(contains(readUserRegistryString(kRunKey, kRunValue).value_or(""), "--check-for-updates"),
          "the update check at logon");
#endif

    request.progress = nullptr;

    // Again: replaces the installation.
    (void)writeFileAtomic(folder / "stale.txt", "old");
    check(install(request).ok() && !exists(folder / "stale.txt"), "reinstalling replaces the folder");

    // A second version next to it.
    const std::vector<std::byte> rcZip = load("ClannectFramework-1.0.0-rc.1-windows-x64-mingw.zip");
    const Package rc = openPackage(rcZip).value();
    request.package = &rc;
    request.tag = "v1.0.0-rc.1";
    check(install(request).ok(), "a second version installs beside the first");
    checkEqual(installedVersions(root).size(), std::size_t{2}, "both are listed");
    checkEqual(installedVersions(root).front().version.toString(), String("1.0.0-rc.1"), "newest first");

    // A foreign folder in the way is never overwritten.
    const Path foreign = temp.value().path() / "Other";
    (void)createDirectories(foreign / "0.9.0");
    (void)writeFileAtomic(foreign / "0.9.0/precious.txt", "mine");
    request.root = foreign;
    request.package = &package;
    const Result<Path> refused = install(request);
    check(!refused && refused.error().code() == ErrorCode::AlreadyExists, "a folder that is not ours is refused");
    check(readTextFile(foreign / "0.9.0/precious.txt").valueOr("") == "mine", "and left alone");

    // Cancelling.
    request.root = temp.value().path() / "Cancelled";
    request.progress = [](double, StringView) { return false; };
    const Result<Path> cancelled = install(request);
    check(!cancelled && cancelled.error().code() == ErrorCode::Cancelled, "progress can cancel");
    check(!exists(request.root / "0.9.0"), "a cancelled install leaves no version folder");

    // Uninstalling one, then the other.
    check(uninstall(folder).ok() && !exists(folder), "uninstalling removes the version");
    check(isFile(maintenanceTool(root)), "the maintenance tool stays for the other version");
#ifdef _WIN32
    check(!readUserRegistryString(key, "DisplayName"), "and its Installed apps entry is gone");
    check(!readUserRegistryString(kCMakePackagesKey, "ClannectFramework-0.9.0"), "as is its CMake registration");
    check(readUserRegistryString(kRunKey, kRunValue).has_value(), "the update check stays");
#endif
    check(uninstall(versionFolder(root, v("1.0.0-rc.1"))).ok(), "the last version uninstalls");
    check(!exists(root), "with the last one the maintenance tool and the root go");
#ifdef _WIN32
    check(!readUserRegistryString(kRunKey, kRunValue), "and the update check");
#endif
    check(!uninstall(foreign / "0.9.0"), "a folder without installed.json is not uninstalled");
    check(isFile(foreign / "0.9.0/precious.txt"), "nor touched");
}

void quoting() {
    checkEqual(quoteArgument("C:\\Program Files\\x.exe"), String("\"C:\\Program Files\\x.exe\""), "spaces");
    checkEqual(quoteArgument("C:\\dir\\"), String("\"C:\\dir\\\\\""), "a trailing backslash is doubled");
    checkEqual(quoteArgument("a\"b"), String("\"a\\\"b\""), "a quote is escaped");
}

// A local HTTP server for the downloads.
struct Server {
    std::unique_ptr<TcpListener> listener;
    std::vector<std::unique_ptr<TcpConnection>> connections;
    std::map<String, String> routes;

    explicit Server(EventLoop &loop) {
        listener = TcpListener::listen(loop, "127.0.0.1", 0).value();
        listener->setOnAccept([this](std::unique_ptr<TcpConnection> c) {
            TcpConnection *raw = c.get();
            auto buffer = std::make_shared<String>();
            raw->setOnData([this, raw, buffer](Span<const std::byte> d) {
                buffer->append(reinterpret_cast<const char *>(d.data()), d.size());
                const auto parsed = parseHttpRequestHead(*buffer);
                if (!parsed || !parsed.value()) {
                    return;
                }
                const auto it = routes.find(parsed.value()->head.target);
                raw->send(it == routes.end() ? String("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n") : it->second);
                raw->close();
            });
            connections.push_back(std::move(c));
        });
    }
    [[nodiscard]] String url(StringView path) const { return "http://127.0.0.1:" + std::to_string(listener->port()) + String(path); }
};

String ok(StringView body) {
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + String(body);
}

void downloads() {
    auto loop = EventLoop::create().value();
    Executor resolver(1);
    HttpClient client(*loop, resolver);
    Server server(*loop);
    const std::vector<std::byte> zip = load("ClannectFramework-0.9.0-windows-x64-mingw.zip");
    const String body(reinterpret_cast<const char *>(zip.data()), zip.size());
    server.routes["/releases"] = ok(readTextFile(kData / "releases.json").valueOr(""));
    server.routes["/limited"] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 2\r\n\r\n{}";
    server.routes["/asset"] = "HTTP/1.1 302 Found\r\nLocation: " + server.url("/objects/pkg") + "\r\nContent-Length: 0\r\n\r\n";
    server.routes["/objects/pkg"] = ok(body);
    server.routes["/short"] = ok(body.substr(0, 1000));

    const auto run = [&](auto &&start) {
        bool finished = false;
        auto handle = start(finished);
        for (int i = 0; i < 500 && !finished; ++i) {
            loop->runOnce(std::chrono::milliseconds(10));
        }
        check(finished, "the request finished");
    };

    run([&](bool &finished) {
        return fetchReleases(client, Url::parse(server.url("/releases")).value(), [&](Result<std::vector<Release>> r) {
            check(r.ok() && r.value().size() == 4, "the releases list downloads and parses");
            finished = true;
        });
    });
    run([&](bool &finished) {
        return fetchReleases(client, Url::parse(server.url("/limited")).value(), [&](Result<std::vector<Release>> r) {
            check(!r && contains(r.error().message(), "limiting"), "a 403 explains GitHub's rate limit");
            finished = true;
        });
    });

    Asset asset;
    asset.url = server.url("/asset");
    asset.size = zip.size();
    asset.sha256 = Sha256::toHex(Sha256::hash(zip));
    std::uint64_t lastReceived = 0;
    run([&](bool &finished) {
        return downloadAsset(
            client, asset, [&](std::uint64_t received, std::uint64_t total) {
                check(received >= lastReceived && total == zip.size(), "download progress");
                lastReceived = received;
            },
            [&](Result<std::vector<std::byte>> r) {
                check(r.ok() && r.value() == zip, "the package downloads through a redirect, checksum verified");
                finished = true;
            });
    });
    checkEqual(lastReceived, static_cast<std::uint64_t>(zip.size()), "progress reached the size");

    Asset tampered = asset;
    tampered.sha256[0] = tampered.sha256[0] == '0' ? '1' : '0';
    run([&](bool &finished) {
        return downloadAsset(client, tampered, nullptr, [&](Result<std::vector<std::byte>> r) {
            check(!r && r.error().code() == ErrorCode::Corrupt, "a checksum mismatch is refused");
            finished = true;
        });
    });
    Asset truncated = asset;
    truncated.url = server.url("/short");
    run([&](bool &finished) {
        return downloadAsset(client, truncated, nullptr, [&](Result<std::vector<std::byte>> r) {
            check(!r && r.error().code() == ErrorCode::Corrupt, "a short download is refused");
            finished = true;
        });
    });
    Asset missing = asset;
    missing.url = server.url("/nothing");
    run([&](bool &finished) {
        return downloadAsset(client, missing, nullptr, [&](Result<std::vector<std::byte>> r) {
            check(!r && contains(r.error().message(), "404"), "a 404 is an error");
            finished = true;
        });
    });
}

} // namespace

int main() {
    versions();
    releases();
    packages();
    installing();
    quoting();
    downloads();
    return cfw::test::finish("InstallerTest");
}
