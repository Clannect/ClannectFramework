#pragma once

// System-wide keyboard shortcuts: keys that reach the application while
// another one has the keyboard (a game, a full-screen video), for "save a
// clip" or "start recording".
//
//     auto keys = GlobalShortcuts::start("dev.example.App",
//                                        {{"clip", "Save a clip", "F8"}, {"rec", "Record", "Alt+F9"}});
//     ...every pass of the main loop:
//     for (const String &id : keys->takeActivated()) { ... }
//
// Linux: the freedesktop portal's GlobalShortcuts (KDE Plasma, GNOME 48+,
// Hyprland...). The first time, the desktop asks the user to allow the
// shortcuts and may let them pick other keys; it remembers the answer.
// Windows: RegisterHotKey (fails for a key another program holds).
// Elsewhere: Failed with the reason.
//
// Registration happens on a thread of its own, so start() returns at once;
// state() says how it went. While a shortcut is registered the system
// delivers it here and not to the focused window.
//
// Threads: takeActivated(), state() and error() from any thread.

#include <memory>
#include <vector>

#include "cfw/core/String.h"

namespace cfw {

struct GlobalShortcut {
    String id;          // "clip"
    String description; // "Save a clip" (the desktop shows it)
    String trigger;     // "F8", "Alt+F9", "Ctrl+Shift+S"
};

class GlobalShortcuts {
public:
    enum class State { Registering, Active, Failed };

    // `applicationId` names the application to the desktop (reverse DNS,
    // like its .desktop file: "dev.clannect.Capture").
    [[nodiscard]] static std::unique_ptr<GlobalShortcuts> start(String applicationId,
                                                               std::vector<GlobalShortcut> shortcuts);
    ~GlobalShortcuts();
    GlobalShortcuts(const GlobalShortcuts &) = delete;
    GlobalShortcuts &operator=(const GlobalShortcuts &) = delete;

    // The ids of the shortcuts pressed since the last call, in order.
    [[nodiscard]] std::vector<String> takeActivated();
    [[nodiscard]] State state() const;
    // Why it failed (empty otherwise).
    [[nodiscard]] String error() const;

    struct Impl;

private:
    explicit GlobalShortcuts(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace cfw
