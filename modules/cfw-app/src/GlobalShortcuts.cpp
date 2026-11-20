// GlobalShortcuts: the freedesktop portal on Linux, RegisterHotKey on
// Windows, Failed elsewhere. Each registers and listens on a thread of its own.

#include "cfw/app/GlobalShortcuts.h"

#include <atomic>
#include <cctype>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <poll.h>

#include "linux/DBus.h"
#include "linux/Portal.h"
#endif

namespace cfw {

namespace {

// "Ctrl+Shift+F8" as its modifiers and key name ("F8").
struct Trigger {
    bool ctrl = false, alt = false, shift = false, meta = false;
    String key;
};

Trigger parseTrigger(const String &text) {
    Trigger t;
    std::size_t from = 0;
    while (from <= text.size()) {
        std::size_t plus = text.find('+', from);
        if (plus == from && plus + 1 == text.size()) {
            plus = String::npos; // "Ctrl++": the key is "+"
        }
        String part = text.substr(from, plus == String::npos ? String::npos : plus - from);
        String lower = part;
        for (char &c : lower) {
            c = char(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lower == "ctrl" || lower == "control") {
            t.ctrl = true;
        } else if (lower == "alt") {
            t.alt = true;
        } else if (lower == "shift") {
            t.shift = true;
        } else if (lower == "meta" || lower == "super" || lower == "win" || lower == "logo") {
            t.meta = true;
        } else {
            t.key = part;
        }
        if (plus == String::npos) {
            break;
        }
        from = plus + 1;
    }
    return t;
}

} // namespace

struct GlobalShortcuts::Impl {
    String applicationId;
    std::vector<GlobalShortcut> shortcuts;
    mutable std::mutex mutex;
    std::vector<String> activated;
    State state = State::Registering;
    String error;
    std::atomic<bool> stop{false};
    std::thread thread;

    void fail(String why) {
        std::lock_guard lock(mutex);
        state = State::Failed;
        error = std::move(why);
    }
    void active() {
        std::lock_guard lock(mutex);
        state = State::Active;
    }
    void pressed(String id) {
        std::lock_guard lock(mutex);
        activated.push_back(std::move(id));
    }
    void run();
};

#if defined(__linux__)

void GlobalShortcuts::Impl::run() {
    using dbus::Connection;
    using dbus::Message;
    using dbus::Value;
    using namespace portal;
    auto connected = Connection::session(kCallTimeout);
    if (!connected) {
        fail(connected.error().message());
        return;
    }
    Connection &bus = *connected.value();
    const char *iface = "org.freedesktop.portal.GlobalShortcuts";
    if (property(bus, iface, "version").value_or(0) == 0) {
        fail("this desktop offers no global shortcuts portal");
        return;
    }
    // Newer portals learn an unsandboxed application's id this way (before
    // any other call); older ones do not have it.
    static_cast<void>(bus.call(Message::methodCall(kPortal, kPath, "org.freedesktop.host.portal.Registry", "Register",
                                                   {Value::string(applicationId), options({})}),
                               kCallTimeout));
    String session;
    {
        const String token = newToken();
        auto r = request(bus, iface, "CreateSession",
                         {options({entry("handle_token", Value::string(token)),
                                   entry("session_handle_token", Value::string(newToken()))})},
                         token, std::chrono::seconds(30), &stop);
        if (!r) {
            fail(r.error().message());
            return;
        }
        session = r.value()["session_handle"].text;
    }
    {
        // The portal's trigger names: "ALT+F9", "CTRL+SHIFT+s", "LOGO+x".
        std::vector<Value> list;
        for (const GlobalShortcut &s : shortcuts) {
            const Trigger t = parseTrigger(s.trigger);
            String trigger;
            for (const auto &[on, name] : {std::pair{t.ctrl, "CTRL"}, std::pair{t.alt, "ALT"}, std::pair{t.shift, "SHIFT"},
                                           std::pair{t.meta, "LOGO"}}) {
                if (on) {
                    trigger += String(name) + "+";
                }
            }
            trigger += t.key;
            list.push_back(Value::structure(
                {Value::string(s.id), options({entry("description", Value::string(s.description)),
                                               entry("preferred_trigger", Value::string(trigger))})}));
        }
        const String token = newToken();
        // The desktop may ask the user here: give them time.
        auto r = request(bus, iface, "BindShortcuts",
                         {Value::objectPath(session), Value::array("(sa{sv})", std::move(list)), Value::string(""),
                          options({entry("handle_token", Value::string(token))})},
                         token, std::chrono::seconds(600), &stop);
        if (!r) {
            fail(r.error().code() == ErrorCode::Cancelled ? String("the shortcuts were not allowed") : r.error().message());
            return;
        }
    }
    static_cast<void>(bus.call(Message::methodCall("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                                   "AddMatch",
                                                   {Value::string("type='signal',interface='org.freedesktop.portal."
                                                                  "GlobalShortcuts',member='Activated'")}),
                               kCallTimeout));
    bus.onSignal = [&](const Message &m) {
        if (m.member == "Activated" && m.body.size() >= 2 && m.body[0].text == session) {
            pressed(m.body[1].text);
        }
    };
    active();
    while (!stop) {
        if (!bus.dispatch()) {
            fail("the session bus went away");
            break;
        }
        pollfd p{bus.fd(), POLLIN, 0};
        ::poll(&p, 1, 200);
    }
    bus.onSignal = nullptr;
    static_cast<void>(bus.call(Message::methodCall(kPortal, session, "org.freedesktop.portal.Session", "Close"),
                               std::chrono::seconds(2)));
}

#elif defined(_WIN32)

void GlobalShortcuts::Impl::run() {
    int registered = 0;
    String refused;
    for (std::size_t i = 0; i < shortcuts.size(); ++i) {
        const Trigger t = parseTrigger(shortcuts[i].trigger);
        UINT vk = 0;
        String k = t.key;
        for (char &c : k) {
            c = char(std::toupper(static_cast<unsigned char>(c)));
        }
        if (k.size() >= 2 && k[0] == 'F' && std::isdigit(static_cast<unsigned char>(k[1]))) {
            const int n = std::atoi(k.c_str() + 1);
            if (n >= 1 && n <= 24) {
                vk = UINT(VK_F1 + n - 1);
            }
        } else if (k.size() == 1 && std::isalnum(static_cast<unsigned char>(k[0]))) {
            vk = UINT(k[0]);
        } else if (k == "SPACE") {
            vk = VK_SPACE;
        } else if (k == "PRINT" || k == "PRINTSCREEN") {
            vk = VK_SNAPSHOT;
        }
        const UINT mods = (t.ctrl ? MOD_CONTROL : 0) | (t.alt ? MOD_ALT : 0) | (t.shift ? MOD_SHIFT : 0) |
                          (t.meta ? MOD_WIN : 0) | MOD_NOREPEAT;
        if (vk != 0 && RegisterHotKey(nullptr, int(i + 1), mods, vk)) {
            ++registered;
        } else {
            refused += (refused.empty() ? "" : ", ") + shortcuts[i].trigger;
        }
    }
    if (registered == 0) {
        fail("no shortcut could be registered" + (refused.empty() ? String() : " (" + refused + ")"));
        return;
    }
    if (!refused.empty()) {
        std::lock_guard lock(mutex);
        error = refused + " is taken by another program";
    }
    active();
    MSG msg;
    while (!stop) {
        // Wakes for a hot key, and every 200 ms to see whether to stop.
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 200, QS_ALLINPUT);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY && msg.wParam >= 1 && msg.wParam <= shortcuts.size()) {
                pressed(shortcuts[msg.wParam - 1].id);
            }
        }
    }
    for (std::size_t i = 0; i < shortcuts.size(); ++i) {
        UnregisterHotKey(nullptr, int(i + 1));
    }
}

#else

void GlobalShortcuts::Impl::run() { fail("global shortcuts are not available on this system"); }

#endif

GlobalShortcuts::GlobalShortcuts(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

std::unique_ptr<GlobalShortcuts> GlobalShortcuts::start(String applicationId, std::vector<GlobalShortcut> shortcuts) {
    auto impl = std::make_unique<Impl>();
    impl->applicationId = std::move(applicationId);
    impl->shortcuts = std::move(shortcuts);
    Impl *raw = impl.get();
    impl->thread = std::thread([raw] { raw->run(); });
    return std::unique_ptr<GlobalShortcuts>(new GlobalShortcuts(std::move(impl)));
}

GlobalShortcuts::~GlobalShortcuts() {
    m_impl->stop = true;
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
}

std::vector<String> GlobalShortcuts::takeActivated() {
    std::lock_guard lock(m_impl->mutex);
    std::vector<String> out;
    out.swap(m_impl->activated);
    return out;
}

GlobalShortcuts::State GlobalShortcuts::state() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->state;
}

String GlobalShortcuts::error() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->error;
}

} // namespace cfw
