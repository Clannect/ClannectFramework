#include "Wizard.h"

#include <algorithm>
#include <cstdio>

#include "cfw/app/Desktop.h"
#include "cfw/core/Strings.h"
#include "cfw/platform/Window.h"

namespace cfw::installer {

namespace {

String megabytes(std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

String displayPath(const Path &path) {
    String text = path.toString();
#ifdef _WIN32
    std::replace(text.begin(), text.end(), '/', '\\');
#endif
    return text;
}

String releaseLabel(const Release &r) {
    String label = r.version.toString();
    if (r.prerelease) {
        label += "  (pre-release)";
    }
    if (r.publishedAt.size() >= 10) {
        label += "  " + r.publishedAt.substr(0, 10);
    }
    return label;
}

Label &paragraph(Stack &column, String text, bool muted = false) {
    auto &label = column.add<Label>(std::move(text));
    label.setWrap(true);
    label.setMuted(muted);
    return label;
}

} // namespace

Wizard::Wizard(UiWindow &window, EventLoop &loop, Executor &resolver, WizardSetup setup)
    : m_ui(window), m_loop(loop), m_client(loop, resolver), m_setup(std::move(setup)), m_root(m_setup.root) {
    if (m_setup.releases) {
        m_releases = *m_setup.releases;
    }
    const bool anyStable = std::any_of(m_releases.begin(), m_releases.end(),
                                       [](const Release &r) { return !r.prerelease && r.windowsPackage; });
    m_showPrereleases = !anyStable;
    m_includePrereleases = true; // everything before 1.0 is a pre-release
    refreshListed();

    Surface &surface = window.surface();
    auto &frame = surface.root().add<Stack>(Stack::Direction::Column, 0.0f);

    auto &header = frame.add<Panel>(Stack::Direction::Column, 4.0f, 20.0f);
    header.setBorder(Panel::Border::Bottom);
    header.add<Label>(m_setup.kind == WizardSetup::Kind::Update ? "Clannect Framework Update"
                                                                 : "Clannect Framework Setup");
    m_subtitle = &header.add<Label>();
    m_subtitle->setMuted(true);

    m_content = &frame.add<Stack>(Stack::Direction::Column, 12.0f, 24.0f);
    m_content->setStretch(1);

    auto &buttons = frame.add<Panel>(Stack::Direction::Row, 8.0f, 12.0f);
    buttons.setBorder(Panel::Border::Top);
    buttons.add<Element>().setStretch(1);
    m_back = &buttons.add<Button>("Back");
    m_next = &buttons.add<Button>("Next");
    m_next->setPrimary(true);
    m_cancel = &buttons.add<Button>("Cancel");
    m_frame.push_back(m_back->clicked.connect([this] { back(); }));
    m_frame.push_back(m_next->clicked.connect([this] { next(); }));
    m_frame.push_back(m_cancel->clicked.connect([this] {
        if (m_busy) {
            m_cancelled = true; // extraction stops at its next file
            if (m_request.pending()) {
                m_request.cancel();
                fail("The download was cancelled.");
            }
            return;
        }
        m_ui.close();
    }));

    if (online() && !m_setup.releases) {
        fetchReleasesList();
    }
    show(Page::Welcome);
}

Wizard::~Wizard() = default;

std::vector<Wizard::Page> Wizard::pages() const {
    std::vector<Page> list{Page::Welcome};
    if (m_setup.kind == WizardSetup::Kind::Online) {
        list.push_back(Page::Version);
    }
    if (!m_setup.rootFixed) {
        list.push_back(Page::Folder);
    }
    if (m_setup.kind != WizardSetup::Kind::Update) {
        list.push_back(Page::Options);
    }
    list.push_back(Page::Installing);
    return list;
}

const Release *Wizard::chosenRelease() const {
    if (m_setup.kind == WizardSetup::Kind::Update) {
        return m_setup.offered ? &*m_setup.offered : nullptr;
    }
    if (m_versionIndex >= 0 && static_cast<std::size_t>(m_versionIndex) < m_listed.size()) {
        return m_listed[static_cast<std::size_t>(m_versionIndex)];
    }
    return nullptr;
}

String Wizard::versionText() const {
    if (!online()) {
        return m_setup.package->version.toString();
    }
    const Release *r = chosenRelease();
    return r ? r->version.toString() : String("?");
}

void Wizard::show(Page page) {
    m_page = page;
    m_connections.clear();
    m_versionList = nullptr;
    m_versionNote = nullptr;
    m_folderField = nullptr;
    m_folderNote = nullptr;
    m_bar = nullptr;
    m_status = nullptr;
    while (!m_content->children().empty()) {
        (void)m_content->remove(*m_content->children().back());
    }
    m_back->setVisible(true);
    m_next->setVisible(true);
    m_cancel->setVisible(true);
    m_back->setEnabled(page != Page::Welcome);
    m_next->setEnabled(true);
    m_next->setText("Next");
    m_cancel->setText("Cancel");
    switch (page) {
    case Page::Welcome: buildWelcome(); break;
    case Page::Version: buildVersion(); break;
    case Page::Folder: buildFolder(); break;
    case Page::Options: buildOptions(); break;
    case Page::Installing: buildInstalling(); break;
    case Page::Done: buildDone(); break;
    case Page::Failed: buildFailed(); break;
    }
    m_content->invalidateLayout();
}

void Wizard::next() {
    if (m_page == Page::Done || m_page == Page::Failed) {
        m_ui.close();
        return;
    }
    if (m_page == Page::Folder) {
        const String text(trim(m_folderField->text()));
        if (text.empty()) {
            m_folderField->setInvalid(true);
            return;
        }
        m_root = Path(text);
    }
    const std::vector<Page> order = pages();
    const auto it = std::find(order.begin(), order.end(), m_page);
    if (it != order.end() && it + 1 != order.end()) {
        show(*(it + 1));
        if (m_page == Page::Installing) {
            startInstall();
        }
    }
}

void Wizard::back() {
    const std::vector<Page> order = pages();
    const auto it = std::find(order.begin(), order.end(), m_page);
    if (it != order.end() && it != order.begin()) {
        show(*(it - 1));
    } else if (m_page == Page::Failed) {
        show(order.size() >= 2 ? order[order.size() - 2] : Page::Welcome);
    }
}

// ---- Pages ----

void Wizard::buildWelcome() {
    Stack &c = *m_content;
    switch (m_setup.kind) {
    case WizardSetup::Kind::Offline:
        m_subtitle->setText("Offline installer for version " + m_setup.package->version.toString());
        paragraph(c, "This installs Clannect Framework " + m_setup.package->version.toString() +
                         " for MinGW-w64 GCC 13 (64-bit): headers, static libraries, the CMake package and the widget "
                         "gallery.");
        paragraph(c, "Everything it installs is inside this program, so it works without an internet connection. It "
                     "installs this version only and does not check for updates: the online installer does both.",
                  true);
        break;
    case WizardSetup::Kind::Online:
        m_subtitle->setText("Online installer");
        paragraph(c, "This installs Clannect Framework for MinGW-w64 GCC 13 (64-bit): headers, static libraries, the "
                     "CMake package and the widget gallery.");
        paragraph(c, "It downloads the version you choose from GitHub, and can tell you when a new version is "
                     "released. Versions install side by side.",
                  true);
        break;
    case WizardSetup::Kind::Update: {
        const Release &r = *m_setup.offered;
        m_subtitle->setText("A new version is available");
        String have = m_setup.installed.empty() ? String("nothing") : m_setup.installed.front().version.toString();
        paragraph(c, "Clannect Framework " + r.version.toString() + (r.prerelease ? " (a pre-release)" : String()) +
                         " was released" + (r.publishedAt.size() >= 10 ? " on " + r.publishedAt.substr(0, 10) : "") +
                         ". You have " + have + ".");
        paragraph(c, "Installing it keeps the versions you have; uninstall those from Windows' Installed apps when "
                     "you no longer need them.",
                  true);
        auto &notes = c.add<Button>("What's new in " + r.version.toString());
        notes.setFlat(true);
        m_connections.push_back(notes.clicked.connect([url = r.pageUrl] { (void)openUrl(url); }));
        auto &skip = c.add<Button>("Skip this version");
        skip.setFlat(true);
        m_connections.push_back(skip.clicked.connect([this] {
            Preferences prefs = loadPreferences(m_root);
            prefs.skippedTag = m_setup.offered->tag;
            (void)savePreferences(m_root, prefs);
            m_ui.close();
        }));
        m_next->setText("Install " + r.version.toString());
        m_cancel->setText("Later");
        break;
    }
    }
    if (!m_setup.installed.empty() && m_setup.kind != WizardSetup::Kind::Update) {
        String have;
        for (const Installed &i : m_setup.installed) {
            have += (have.empty() ? "" : ", ") + i.version.toString();
        }
        paragraph(c, "Installed now: " + have + ".", true);
    }
    if (m_setup.kind != WizardSetup::Kind::Update) {
        paragraph(c, "Clannect Framework is available under the Open Use License (OUL) v1.1: see license.md after "
                     "installing, or " + String(kProjectUrl) + ".",
                  true);
    }
}

void Wizard::buildVersion() {
    Stack &c = *m_content;
    m_subtitle->setText("Choose a version");
    if (m_fetching || !m_fetchError.empty()) {
        m_next->setEnabled(false);
        if (m_fetching) {
            paragraph(c, "Getting the list of releases from GitHub...");
            auto &bar = c.add<ProgressBar>();
            bar.setBusy(true);
        } else {
            paragraph(c, "Could not get the list of releases: " + m_fetchError);
            auto &retry = c.add<Button>("Try again");
            m_connections.push_back(retry.clicked.connect([this] {
                fetchReleasesList();
                show(Page::Version);
            }));
        }
        return;
    }
    paragraph(c, "Clannect Framework version:");
    m_versionList = &c.add<Dropdown>();
    auto &pre = c.add<CheckBox>("Show pre-releases", m_showPrereleases);
    m_versionNote = &paragraph(c, "", true);
    m_connections.push_back(pre.toggled.connect([this](bool on) {
        m_showPrereleases = on;
        m_versionIndex = 0;
        fillVersions();
    }));
    m_connections.push_back(m_versionList->currentChanged.connect([this](int index) {
        m_versionIndex = index;
        fillVersions();
    }));
    fillVersions();
}

void Wizard::refreshListed() {
    m_listed.clear();
    for (const Release &r : m_releases) {
        if (r.windowsPackage && (m_showPrereleases || !r.prerelease)) {
            m_listed.push_back(&r);
        }
    }
}

void Wizard::fillVersions() {
    refreshListed();
    if (!m_versionList) {
        return;
    }
    std::vector<String> items;
    for (const Release *r : m_listed) {
        items.push_back(releaseLabel(*r));
    }
    m_versionIndex = std::clamp(m_versionIndex, 0, std::max(0, static_cast<int>(items.size()) - 1));
    m_versionList->setItems(items);
    m_versionList->setCurrentIndex(items.empty() ? -1 : m_versionIndex);
    m_versionList->setPlaceholder("No versions");
    const Release *r = chosenRelease();
    m_next->setEnabled(r != nullptr);
    if (r) {
        m_versionNote->setText("Download: " + megabytes(r->windowsPackage->size) +
                               (r->prerelease ? ". Pre-releases (all of 0.x) may change their API between versions." : "."));
    } else {
        m_versionNote->setText(m_showPrereleases ? "No release has a Windows package yet."
                                                 : "No stable release yet: show pre-releases to see the 0.x versions.");
    }
}

void Wizard::buildFolder() {
    Stack &c = *m_content;
    m_subtitle->setText("Choose where to install");
    paragraph(c, "Install folder:");
    auto &row = c.add<Stack>(Stack::Direction::Row, 8.0f);
    m_folderField = &row.add<TextField>(displayPath(m_root));
    m_folderField->setStretch(1);
    auto &browse = row.add<Button>("Browse...");
    m_folderNote = &paragraph(c, "", true);
    const auto update = [this] {
        const String text(trim(m_folderField->text()));
        m_folderField->setInvalid(false);
        m_folderNote->setText("Clannect Framework " + versionText() + " goes into " +
                              displayPath(versionFolder(Path(text), Version::parse(versionText()).value_or(Version{}))) +
                              ". Other versions can live in the same folder, side by side. No administrator rights "
                              "are needed.");
    };
    update();
    m_connections.push_back(m_folderField->textChanged.connect([update](const String &) { update(); }));
    m_connections.push_back(browse.clicked.connect([this, update] {
        ChooseFolderOptions options;
        options.title = "Install Clannect Framework to";
        options.directory = Path(String(trim(m_folderField->text()))).parent();
        const Result<std::optional<Path>> chosen = chooseFolder(&m_ui.window(), options);
        if (chosen && chosen.value()) {
            m_folderField->setText(displayPath(*chosen.value()));
            update();
        }
    }));
}

void Wizard::buildOptions() {
    Stack &c = *m_content;
    m_subtitle->setText("Options");
    auto &cmake = c.add<CheckBox>("Let CMake find this version", m_registerWithCMake);
    paragraph(c, "find_package(ClannectFramework) then works without setting CMAKE_PREFIX_PATH (CMake's user package "
                 "registry).",
              true);
    m_connections.push_back(cmake.toggled.connect([this](bool on) { m_registerWithCMake = on; }));
    if (online()) {
        auto &notify = c.add<CheckBox>("Tell me when a new version is released", m_notifyUpdates);
        paragraph(c, "The maintenance tool checks GitHub once a day when you sign in to Windows, and asks before "
                     "installing anything.",
                  true);
        auto &pre = c.add<CheckBox>("Include pre-releases", m_includePrereleases);
        m_connections.push_back(notify.toggled.connect([this, &pre](bool on) {
            m_notifyUpdates = on;
            pre.setEnabled(on);
        }));
        m_connections.push_back(pre.toggled.connect([this](bool on) { m_includePrereleases = on; }));
        pre.setEnabled(m_notifyUpdates);
    } else {
        paragraph(c, "This offline installer does not check for updates. Use the online installer to be told about "
                     "new versions.",
                  true);
    }
    m_next->setText("Install");
}

void Wizard::buildInstalling() {
    Stack &c = *m_content;
    m_subtitle->setText("Installing Clannect Framework " + versionText());
    m_status = &paragraph(c, "Starting...");
    m_bar = &c.add<ProgressBar>();
    m_back->setEnabled(false);
    m_next->setEnabled(false);
}

void Wizard::buildDone() {
    Stack &c = *m_content;
    m_subtitle->setText("Clannect Framework " + versionText() + " is installed");
    paragraph(c, "Installed to " + displayPath(m_installed) + ".");
    paragraph(c, m_registerWithCMake
                     ? String("In your CMakeLists.txt: find_package(ClannectFramework) and "
                              "target_link_libraries(my_app PRIVATE cfw::app).")
                     : "Point CMake at it with -DCMAKE_PREFIX_PATH=" + displayPath(m_installed) +
                           ", then find_package(ClannectFramework).",
              true);
    paragraph(c, "To uninstall, use Windows' Settings > Apps > Installed apps.", true);
    auto &row = c.add<Stack>(Stack::Direction::Row, 8.0f);
    auto &folder = row.add<Button>("Open the folder");
    auto &gallery = row.add<Button>("Run the widget gallery");
    m_connections.push_back(folder.clicked.connect([this] { (void)showInFileManager(m_installed); }));
    m_connections.push_back(gallery.clicked.connect([this] {
#ifdef _WIN32
        (void)openUrl(displayPath(m_installed / "bin/cfw-ui-gallery.exe"));
#else
        (void)openUrl((m_installed / "bin/cfw-ui-gallery").toString());
#endif
    }));
    m_back->setVisible(false);
    m_cancel->setVisible(false);
    m_next->setText("Finish");
}

void Wizard::buildFailed() {
    Stack &c = *m_content;
    m_subtitle->setText("The installation did not finish");
    paragraph(c, m_error);
    paragraph(c, "Nothing of this version was left half-installed. Go back to try again.", true);
    m_back->setEnabled(true);
    m_cancel->setVisible(false);
    m_next->setText("Close");
}

// ---- Work ----

void Wizard::fetchReleasesList() {
    m_fetching = true;
    m_fetchError.clear();
    m_request = fetchReleases(m_client, m_setup.releasesUrl, [this](Result<std::vector<Release>> result) {
        m_fetching = false;
        if (result) {
            m_releases = std::move(result.value());
            const bool anyStable = std::any_of(m_releases.begin(), m_releases.end(),
                                               [](const Release &r) { return !r.prerelease && r.windowsPackage; });
            m_showPrereleases = !anyStable;
            refreshListed();
        } else {
            m_fetchError = result.error().message();
        }
        if (m_page == Page::Version) {
            show(Page::Version);
        }
    });
}

void Wizard::setStatus(double fraction, StringView text) {
    if (m_bar) {
        m_bar->setValue(static_cast<float>(fraction));
    }
    if (m_status) {
        m_status->setText(String(text));
    }
}

void Wizard::pump() {
    // Extraction runs on this thread: keep the window painted and the
    // Cancel button answering while it does.
    processEvents(Duration::zero());
    m_ui.frame();
}

void Wizard::startInstall() {
    m_cancelled = false;
    m_busy = true;
    m_downloaded.reset();
    if (!online()) {
        installPackage(*m_setup.package);
        return;
    }
    const Release *release = chosenRelease();
    if (!release || !release->windowsPackage) {
        fail("No version was chosen.");
        return;
    }
    setStatus(0, "Downloading " + release->windowsPackage->name + "...");
    m_request = downloadAsset(
        m_client, *release->windowsPackage,
        [this](std::uint64_t received, std::uint64_t total) {
            const double fraction = total ? static_cast<double>(received) / static_cast<double>(total) : 0.0;
            setStatus(0.6 * fraction, "Downloading: " + megabytes(received) + " of " + megabytes(total));
        },
        [this](Result<std::vector<std::byte>> result) {
            if (!result) {
                fail("The download failed: " + result.error().message());
                return;
            }
            m_download = std::move(result.value());
            Result<Package> package = openPackage(m_download);
            if (!package) {
                fail("The download is not a Clannect Framework package: " + package.error().message());
                return;
            }
            m_downloaded = std::move(package.value());
            installPackage(*m_downloaded);
        });
}

void Wizard::installPackage(const Package &package) {
    const double base = online() ? 0.6 : 0.0;
    InstallRequest request;
    request.root = m_root;
    request.package = &package;
    const Release *release = online() ? chosenRelease() : nullptr;
    request.tag = release ? release->tag : "v" + package.version.toString();
    request.prerelease = release ? release->prerelease : true;
    request.maintenanceTool = m_setup.maintenanceTool;
    request.registerWithCMake = m_registerWithCMake;
    if (online()) {
        request.notifyUpdates = m_notifyUpdates;
        request.includePrereleases = m_includePrereleases;
    }
    request.progress = [this, base](double fraction, StringView what) {
        setStatus(base + (1.0 - base) * fraction, what);
        pump();
        return !m_cancelled;
    };
    const Result<Path> installed = install(request);
    m_busy = false;
    if (!installed) {
        fail(installed.error().code() == ErrorCode::Cancelled ? String("The installation was cancelled.")
                                                             : installed.error().message());
        return;
    }
    m_installed = installed.value();
    show(Page::Done);
}

void Wizard::fail(String message) {
    m_busy = false;
    m_error = std::move(message);
    show(Page::Failed);
}

void Wizard::run() {
    while (m_ui.isOpen()) {
        m_loop.runOnce(Duration::zero());
        m_ui.frame();
        processEvents(m_busy ? std::chrono::milliseconds(5) : std::chrono::milliseconds(16));
    }
}

} // namespace cfw::installer
