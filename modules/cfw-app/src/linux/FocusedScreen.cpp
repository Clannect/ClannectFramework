// focusedScreen on Linux. KWin (Plasma, Wayland and X11) has no call that
// answers it directly, so a tiny KWin script is loaded that reads the active
// window's monitor and calls back to this connection over D-Bus; then it is
// unloaded again. Other desktops: Unsupported.

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "DBus.h"
#include "cfw/app/Desktop.h"
#include "cfw/io/FileSystem.h"

namespace cfw {

using dbus::Connection;
using dbus::Message;
using dbus::Value;

namespace {

constexpr auto kTimeout = std::chrono::milliseconds(1500);
constexpr const char *kInterface = "dev.cfw.FocusedScreen";

// A number from the script (JavaScript numbers may arrive as doubles).
int number(const Value &v) { return v.kind == Value::Kind::Double ? int(v.real) : int(v.asInt()); }

} // namespace

Result<FocusedScreen> focusedScreen() {
    auto connected = Connection::session(kTimeout);
    if (!connected) {
        return connected.error();
    }
    Connection &bus = *connected.value();
    // Is KWin's scripting there?
    auto owner = bus.call(Message::methodCall("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                              "NameHasOwner", {Value::string("org.kde.KWin")}),
                          kTimeout);
    if (!owner || owner.value().body.empty() || owner.value().body[0].asInt() == 0) {
        return Error(ErrorCode::Unsupported, "the focused window's screen is known on KDE Plasma and Windows");
    }

    static std::atomic<unsigned> counter{0};
    const String plugin = "cfw_focused_" + std::to_string(::getpid()) + "_" + std::to_string(counter++);
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    const Path script = Path(runtime && *runtime ? runtime : "/tmp") / (plugin + ".js");
    // Plasma 6 (activeWindow, output) with Plasma 5's names as the fallback.
    const String js = "const w = workspace.activeWindow !== undefined ? workspace.activeWindow : workspace.activeClient;\n"
                      "const o = w ? (w.output !== undefined ? w.output : null) : null;\n"
                      "const s = o ? o : (workspace.activeScreen !== undefined && typeof workspace.activeScreen === 'object'"
                      " ? workspace.activeScreen : null);\n"
                      "const g = s ? s.geometry : (w ? w.frameGeometry : {x: 0, y: 0, width: 0, height: 0});\n"
                      "let x0 = g.x, y0 = g.y, x1 = g.x + g.width, y1 = g.y + g.height;\n"
                      "for (const o2 of (workspace.screens || [])) {\n"
                      "  const h = o2.geometry;\n"
                      "  x0 = Math.min(x0, h.x); y0 = Math.min(y0, h.y);\n"
                      "  x1 = Math.max(x1, h.x + h.width); y1 = Math.max(y1, h.y + h.height);\n"
                      "}\n"
                      "callDBus(\"" +
                      bus.uniqueName() + "\", \"/\", \"" + kInterface +
                      "\", \"Report\", s ? String(s.name) : \"\", g.x, g.y, g.width, g.height,"
                      " w ? String(w.resourceClass) : \"\", [x0, y0, x1 - x0, y1 - y0].join(\",\"));\n";
    if (auto w = writeFileAtomic(script, js); !w) {
        return w.error();
    }

    std::optional<FocusedScreen> answer;
    bus.onMethodCall = [&](const Message &call) {
        if (call.interface == kInterface && call.member == "Report" && call.body.size() >= 6) {
            FocusedScreen f;
            f.name = call.body[0].text;
            f.geometry = {number(call.body[1]), number(call.body[2]), number(call.body[3]), number(call.body[4])};
            f.application = call.body[5].text;
            // KWin passes only a few arguments through: the desktop's box as "x,y,w,h".
            if (call.body.size() >= 7) {
                int d[4] = {};
                if (std::sscanf(call.body[6].text.c_str(), "%d,%d,%d,%d", &d[0], &d[1], &d[2], &d[3]) == 4) {
                    f.desktop = {d[0], d[1], d[2], d[3]};
                }
            }
            answer = f;
        }
        if (!(call.flags & 1)) {
            bus.send(Message::methodReturn(call));
        }
    };

    auto unload = [&] {
        static_cast<void>(bus.call(Message::methodCall("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
                                                       "unloadScript", {Value::string(plugin)}),
                                   kTimeout));
        static_cast<void>(removeFile(script));
    };
    auto loaded = bus.call(Message::methodCall("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "loadScript",
                                               {Value::string(script.toString()), Value::string(plugin)}),
                           kTimeout);
    if (!loaded || loaded.value().body.empty() || loaded.value().body[0].asInt() < 0) {
        unload();
        return Error(ErrorCode::Unsupported, "KWin did not load the script");
    }
    const String id = std::to_string(loaded.value().body[0].asInt());
    // Plasma 6 names the script "/Scripting/Script<id>", Plasma 5 "/<id>".
    auto run = bus.call(Message::methodCall("org.kde.KWin", "/Scripting/Script" + id, "org.kde.kwin.Script", "run"),
                        kTimeout);
    if (!run) {
        run = bus.call(Message::methodCall("org.kde.KWin", "/" + id, "org.kde.kwin.Script", "run"), kTimeout);
    }
    const auto end = std::chrono::steady_clock::now() + kTimeout;
    while (run && !answer && std::chrono::steady_clock::now() < end) {
        bus.dispatch();
        if (answer) {
            break;
        }
        pollfd p{bus.fd(), POLLIN, 0};
        ::poll(&p, 1, 50);
    }
    bus.onMethodCall = nullptr;
    unload();
    if (!answer) {
        return Error(ErrorCode::Timeout, "KWin did not say which screen is focused");
    }
    if (answer->geometry.isEmpty()) {
        return Error(ErrorCode::NotFound, "no window is focused");
    }
    return *answer;
}

} // namespace cfw
