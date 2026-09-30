#pragma once

// The installer's window: a wizard of pages (welcome, version, folder,
// options, progress, done) drawn with cfw-ui. The same window offers an
// update when the maintenance tool finds one.

#include <memory>
#include <optional>
#include <vector>

#include "Download.h"
#include "Installer.h"
#include "cfw/app/UiWindow.h"
#include "cfw/net/EventLoop.h"
#include "cfw/net/Executor.h"
#include "cfw/ui/Chrome.h"
#include "cfw/ui/Controls.h"

namespace cfw::installer {

struct WizardSetup {
    enum class Kind { Offline, Online, Update };
    Kind kind = Kind::Online;
    const Package *package = nullptr;         // Offline
    Span<const std::byte> maintenanceTool;    // this program without a package
    Path root;
    bool rootFixed = false;                   // the maintenance tool's own root
    Url releasesUrl;
    std::optional<std::vector<Release>> releases; // already known (Update, tests)
    std::optional<Release> offered;               // Update: the new version
    std::vector<Installed> installed;
};

class Wizard {
public:
    enum class Page { Welcome, Version, Folder, Options, Installing, Done, Failed };

    Wizard(UiWindow &window, EventLoop &loop, Executor &resolver, WizardSetup setup);
    ~Wizard();
    Wizard(const Wizard &) = delete;
    Wizard &operator=(const Wizard &) = delete;

    void show(Page page);
    [[nodiscard]] Page page() const noexcept { return m_page; }
    // The first run of the event loops: until the window closes.
    void run();

private:
    [[nodiscard]] bool online() const { return m_setup.kind != WizardSetup::Kind::Offline; }
    [[nodiscard]] std::vector<Page> pages() const;
    [[nodiscard]] String versionText() const;
    [[nodiscard]] const Release *chosenRelease() const;
    void next();
    void back();
    void fetchReleasesList();
    void fillVersions();
    void refreshListed();
    void startInstall();
    void installPackage(const Package &package);
    void fail(String message);
    void setStatus(double fraction, StringView text);
    void pump();

    void buildWelcome();
    void buildVersion();
    void buildFolder();
    void buildOptions();
    void buildInstalling();
    void buildDone();
    void buildFailed();

    UiWindow &m_ui;
    EventLoop &m_loop;
    HttpClient m_client;
    WizardSetup m_setup;
    Page m_page = Page::Welcome;

    Stack *m_content = nullptr;
    Label *m_subtitle = nullptr;
    Button *m_back = nullptr;
    Button *m_next = nullptr;
    Button *m_cancel = nullptr;
    std::vector<ScopedConnection> m_connections; // the current page's
    std::vector<ScopedConnection> m_frame;       // the window's buttons

    // Choices, kept across pages.
    std::vector<Release> m_releases;
    bool m_fetching = false;
    String m_fetchError;
    bool m_showPrereleases = true;
    int m_versionIndex = 0;
    std::vector<const Release *> m_listed;
    Path m_root;
    bool m_registerWithCMake = true;
    bool m_notifyUpdates = true;
    bool m_includePrereleases = true;

    // The page's controls.
    Dropdown *m_versionList = nullptr;
    Label *m_versionNote = nullptr;
    TextField *m_folderField = nullptr;
    Label *m_folderNote = nullptr;
    ProgressBar *m_bar = nullptr;
    Label *m_status = nullptr;

    ConnectRequest m_request;
    std::vector<std::byte> m_download;
    std::optional<Package> m_downloaded;
    bool m_cancelled = false;
    bool m_busy = false;
    Path m_installed;
    String m_error;
};

} // namespace cfw::installer
