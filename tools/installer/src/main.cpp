// The Clannect Framework installer, online and offline, and the maintenance
// tool: one program. What it does depends on what it carries and how it is
// started (docs/decisions/0017-windows-installer.md).
//
//   (no arguments)            the wizard: offline if a package is appended to
//                             this program, otherwise online (download)
//   --check-for-updates       the maintenance tool at logon: once a day, asks
//                             GitHub for a newer version and offers it
//   --uninstall <folder>      uninstalls one version (Installed apps runs this)
//
// For scripts and tests:
//   --silent                  no window; with --root, --version, --no-cmake,
//                             --no-notify; the exit code says whether it worked
//   --root <folder>           where versions go (default %LOCALAPPDATA%\Programs\ClannectFramework)
//   --version <tag>           online: which release (default: the newest)
//   --releases-url <url>      online: another releases list (a test server)
//   --releases-json <file>    online: the releases list from a file
//   --force                   --check-for-updates even if checked today
//   --screenshot <page> <png> paints a wizard page to a PNG and exits

#include <cstdio>
#include <ctime>

#include "Download.h"
#include "Installer.h"
#include "Wizard.h"
#include "cfw/app/UiWindow.h"
#include "cfw/image/Png.h"
#include "cfw/io/Arguments.h"
#include "cfw/io/Environment.h"
#include "cfw/io/FileSystem.h"
#include "cfw/io/StandardPaths.h"
#include "cfw/ui/Dialog.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef MessageBox // windows.h's macro, not cfw::MessageBox

#include <algorithm>
#include <string>

#include "cfw/core/Utf8.h"
#endif

using namespace cfw;
using namespace cfw::installer;

namespace {

struct Options {
    bool silent = false;
    bool checkForUpdates = false;
    bool force = false;
    bool noCMake = false;
    bool noNotify = false;
    std::optional<Path> uninstallFolder;
    std::optional<Path> root;
    String version;
    String releasesUrl = kReleasesUrl;
    std::optional<Path> releasesJson;
    String screenshotPage;
    Path screenshotFile;
};

void say(const char *format, const String &text) {
    std::fprintf(stdout, format, text.c_str());
    std::fflush(stdout);
}

Path defaultRoot() {
#ifdef _WIN32
    if (const std::optional<String> local = environmentVariable("LOCALAPPDATA")) {
        return Path(*local) / "Programs" / "ClannectFramework";
    }
#endif
    return homeDirectory().valueOr(Path(".")) / ".local/share/ClannectFramework";
}

// The maintenance tool lives in <root>/maintenance: its root is fixed.
std::optional<Path> maintenanceRoot(const Path &self) {
    if (self.parent().fileName() == kMaintenanceFolder) {
        return self.parent().parent();
    }
    return std::nullopt;
}

// Runs the loop until `done` is set (a synchronous fetch for the CLI).
template <class Start> void runUntil(EventLoop &loop, bool &done, Start &&start) {
    auto handle = start();
    const std::time_t deadline = std::time(nullptr) + 120;
    while (!done && std::time(nullptr) < deadline) {
        loop.runOnce(std::chrono::milliseconds(50));
    }
}

Result<std::vector<Release>> releasesNow(EventLoop &loop, HttpClient &client, const Options &options) {
    if (options.releasesJson) {
        return parseReleases(readTextFile(*options.releasesJson).valueOr(""));
    }
    Result<std::vector<Release>> result = Error(ErrorCode::Timeout, "GitHub did not answer");
    bool done = false;
    const Result<Url> url = Url::parse(options.releasesUrl);
    if (!url) {
        return url.error();
    }
    runUntil(loop, done, [&] {
        return fetchReleases(client, url.value(), [&](Result<std::vector<Release>> r) {
            result = std::move(r);
            done = true;
        });
    });
    return result;
}

Result<std::vector<std::byte>> downloadNow(EventLoop &loop, HttpClient &client, const Asset &asset) {
    Result<std::vector<std::byte>> result = Error(ErrorCode::Timeout, "the download did not finish");
    bool done = false;
    runUntil(loop, done, [&] {
        return downloadAsset(client, asset, nullptr, [&](Result<std::vector<std::byte>> r) {
            result = std::move(r);
            done = true;
        });
    });
    return result;
}

#ifdef _WIN32
// The maintenance tool cannot delete itself while it runs: after the last
// uninstall, a hidden command waits for it to exit and removes it.
void removeAfterExit(const Path &folder) {
    String target = folder.toString();
    std::replace(target.begin(), target.end(), '/', '\\');
    const String command = "cmd.exe /d /c ping -n 3 127.0.0.1 >nul & rmdir /s /q " + quoteArgument(target) +
                           " & rmdir " + quoteArgument(Path(target).parent().toString());
    const Result<std::u16string> wide = utf8ToUtf16(command);
    if (!wide) {
        return;
    }
    std::wstring line(wide.value().begin(), wide.value().end());
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                       nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}
#endif

std::unique_ptr<UiWindow> openWindow(const char *title) {
    auto created = UiWindow::create({title, {640, 460}}, Theme::dark().withSystemFonts());
    if (!created) {
        std::fprintf(stderr, "cfw-installer: %s\n", created.error().message().c_str());
        return nullptr;
    }
    return std::move(created).value();
}

int uninstallMode(const Options &options, const Path &self) {
    const Path folder = *options.uninstallFolder;
    if (!options.silent) {
        auto ui = openWindow("Uninstall Clannect Framework");
        if (!ui) {
            return 1;
        }
        int answer = -1;
        MessageBox::show(ui->surface(), "Uninstall Clannect Framework",
                         "Remove Clannect Framework from " + folder.toString() + "?", {"Uninstall", "Cancel"},
                         [&](int button) {
                             answer = button;
                             ui->close();
                         });
        while (ui->isOpen()) {
            ui->frame();
            processEvents(std::chrono::milliseconds(16));
        }
        if (answer != 0) {
            return 2;
        }
    }
    const Path root = folder.parent();
    const Result<void> removed = uninstall(folder);
    if (!removed) {
        say("cfw-installer: %s\n", removed.error().message());
        return 1;
    }
    say("uninstalled %s\n", folder.toString());
#ifdef _WIN32
    if (maintenanceRoot(self) && installedVersions(root).empty() && exists(self)) {
        removeAfterExit(self.parent());
    }
#else
    (void)self;
    (void)root;
#endif
    return 0;
}

int silentInstall(const Options &options, const std::optional<Package> &package, Span<const std::byte> tool,
                  EventLoop &loop, HttpClient &client) {
    InstallRequest request;
    request.root = options.root.value_or(defaultRoot());
    request.maintenanceTool = tool;
    request.registerWithCMake = !options.noCMake;
    std::vector<std::byte> download;
    std::optional<Package> fetched;
    if (package) {
        request.package = &*package;
        request.tag = "v" + package->version.toString();
        request.prerelease = true;
    } else {
        const Result<std::vector<Release>> releases = releasesNow(loop, client, options);
        if (!releases) {
            say("cfw-installer: %s\n", releases.error().message());
            return 1;
        }
        const Release *chosen = nullptr;
        for (const Release &r : releases.value()) {
            if (r.windowsPackage && (options.version.empty() || r.tag == options.version ||
                                     r.version.toString() == options.version)) {
                chosen = &r;
                break;
            }
        }
        if (!chosen) {
            say("cfw-installer: no release %s with a Windows package\n", options.version);
            return 1;
        }
        say("downloading %s\n", chosen->windowsPackage->url);
        Result<std::vector<std::byte>> bytes = downloadNow(loop, client, *chosen->windowsPackage);
        if (!bytes) {
            say("cfw-installer: %s\n", bytes.error().message());
            return 1;
        }
        download = std::move(bytes.value());
        Result<Package> opened = openPackage(download);
        if (!opened) {
            say("cfw-installer: %s\n", opened.error().message());
            return 1;
        }
        fetched = std::move(opened.value());
        request.package = &*fetched;
        request.tag = chosen->tag;
        request.prerelease = chosen->prerelease;
        request.notifyUpdates = !options.noNotify;
    }
    const Result<Path> installed = install(request);
    if (!installed) {
        say("cfw-installer: %s\n", installed.error().message());
        return 1;
    }
    say("installed %s\n", installed.value().toString());
    return 0;
}

int checkForUpdates(const Options &options, const Path &root, Span<const std::byte> tool, EventLoop &loop,
                    Executor &resolver, HttpClient &client) {
    Preferences prefs = loadPreferences(root);
    const std::vector<Installed> installed = installedVersions(root);
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    if (!prefs.notifyUpdates || installed.empty() ||
        (!options.force && now - prefs.lastUpdateCheck < 20 * 60 * 60)) {
        return 0;
    }
    const Result<std::vector<Release>> releases = releasesNow(loop, client, options);
    if (!releases) {
        return 0; // offline or rate-limited: try again at the next logon
    }
    prefs.lastUpdateCheck = now;
    (void)savePreferences(root, prefs);
    const std::optional<Release> newer = newerRelease(releases.value(), installed, prefs);
    if (!newer) {
        say("up to date (%s)\n", installed.front().version.toString());
        return 0;
    }
    say("new version: %s\n", newer->tag);
    if (options.silent) {
        return 3;
    }
    auto ui = openWindow("Clannect Framework Update");
    if (!ui) {
        return 1;
    }
    WizardSetup setup;
    setup.kind = WizardSetup::Kind::Update;
    setup.maintenanceTool = tool;
    setup.root = root;
    setup.rootFixed = true;
    setup.releasesUrl = Url::parse(options.releasesUrl).valueOr(Url{});
    setup.releases = releases.value();
    setup.offered = newer;
    setup.installed = installed;
    Wizard wizard(*ui, loop, resolver, std::move(setup));
    wizard.run();
    return 0;
}

int screenshot(UiWindow &ui, Wizard &wizard, const Options &options) {
    const std::pair<const char *, Wizard::Page> names[] = {
        {"welcome", Wizard::Page::Welcome}, {"version", Wizard::Page::Version}, {"folder", Wizard::Page::Folder},
        {"options", Wizard::Page::Options}, {"installing", Wizard::Page::Installing}, {"done", Wizard::Page::Done},
        {"failed", Wizard::Page::Failed}};
    for (const auto &[name, page] : names) {
        if (options.screenshotPage == name) {
            wizard.show(page);
        }
    }
    for (int i = 0; i < 30; ++i) {
        processEvents(std::chrono::milliseconds(10));
        ui.frame();
    }
    const Result<Image> shot = ui.window().capture();
    const Result<std::vector<std::byte>> png = shot ? encodePng(shot.value()) : Result<std::vector<std::byte>>(shot.error());
    if (!png || !writeFileAtomic(options.screenshotFile, png.value())) {
        std::fprintf(stderr, "cfw-installer: could not write the screenshot\n");
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    const std::vector<String> args = processArguments(argc, argv);
    Options options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const String &a = args[i];
        const bool hasValue = i + 1 < args.size();
        if (a == "--silent") {
            options.silent = true;
        } else if (a == "--check-for-updates") {
            options.checkForUpdates = true;
        } else if (a == "--force") {
            options.force = true;
        } else if (a == "--no-cmake") {
            options.noCMake = true;
        } else if (a == "--no-notify") {
            options.noNotify = true;
        } else if (a == "--uninstall" && hasValue) {
            options.uninstallFolder = Path(args[++i]);
        } else if (a == "--root" && hasValue) {
            options.root = Path(args[++i]);
        } else if (a == "--version" && hasValue) {
            options.version = args[++i];
        } else if (a == "--releases-url" && hasValue) {
            options.releasesUrl = args[++i];
        } else if (a == "--releases-json" && hasValue) {
            options.releasesJson = Path(args[++i]);
        } else if (a == "--screenshot" && i + 2 < args.size()) {
            options.screenshotPage = args[++i];
            options.screenshotFile = Path(args[++i]);
        } else {
            std::fprintf(stderr, "cfw-installer: unknown argument %s\n", a.c_str());
            return 64;
        }
    }
#ifdef _WIN32
    if (options.silent || options.checkForUpdates) {
        AttachConsole(ATTACH_PARENT_PROCESS); // a GUI program printing to the console it was started from
        std::freopen("CONOUT$", "w", stdout);
    }
#endif

    // This program, and the package appended to it (the offline installer).
    const Result<Path> self = executablePath();
    const Result<std::vector<std::byte>> bytes = self ? readFile(self.value(), 1u << 30) : Result<std::vector<std::byte>>(self.error());
    if (!bytes) {
        std::fprintf(stderr, "cfw-installer: cannot read itself: %s\n", bytes.error().message().c_str());
        return 1;
    }
    Result<std::optional<Package>> found = findPackage(bytes.value());
    if (!found) {
        std::fprintf(stderr, "cfw-installer: the attached package is damaged: %s\n", found.error().message().c_str());
        return 1;
    }
    const std::optional<Package> &package = found.value();
    const Span<const std::byte> tool =
        package ? Span<const std::byte>(bytes.value().data(), package->prefixSize) : Span<const std::byte>(bytes.value());
    const std::optional<Path> fixedRoot = maintenanceRoot(self.value());
    const Path root = options.root.value_or(fixedRoot.value_or(defaultRoot()));

    if (options.uninstallFolder) {
        return uninstallMode(options, self.value());
    }

    auto loop = EventLoop::create();
    if (!loop) {
        std::fprintf(stderr, "cfw-installer: %s\n", loop.error().message().c_str());
        return 1;
    }
    Executor resolver(1);
    HttpClient client(*loop.value(), resolver);

    if (options.checkForUpdates) {
        return checkForUpdates(options, root, tool, *loop.value(), resolver, client);
    }
    if (options.silent) {
        Options silent = options;
        silent.root = root;
        return silentInstall(silent, package, tool, *loop.value(), client);
    }

    auto ui = openWindow("Clannect Framework Setup");
    if (!ui) {
        return 1;
    }
    WizardSetup setup;
    setup.kind = package ? WizardSetup::Kind::Offline : WizardSetup::Kind::Online;
    setup.package = package ? &*package : nullptr;
    setup.maintenanceTool = tool;
    setup.root = root;
    setup.rootFixed = fixedRoot.has_value() && !options.root;
    setup.releasesUrl = Url::parse(options.releasesUrl).valueOr(Url{});
    if (options.releasesJson) {
        Result<std::vector<Release>> list = parseReleases(readTextFile(*options.releasesJson).valueOr(""));
        if (list) {
            setup.releases = std::move(list.value());
        }
    }
    setup.installed = installedVersions(root);
    Wizard wizard(*ui, *loop.value(), resolver, std::move(setup));
    if (!options.screenshotPage.empty()) {
        return screenshot(*ui, wizard, options);
    }
    wizard.run();
    return 0;
}
