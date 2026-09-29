// Screen readers on Linux: a window's surface through AT-SPI, read by
// pyatspi (Orca's library) over a private accessibility bus with the real
// registry (at-spi2-registryd). The application and its window are found;
// the tree has the controls with their roles, names, states and text; the
// default action presses a button, text and values are set, extents are on
// the screen, a tree item expands, and a focus change is an event.
// Skips (and passes) without a display, dbus-daemon, the registry or
// pyatspi.

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "../src/linux/DBus.h"
#include "cfw/app/UiWindow.h"
#include "cfw/platform/Window.h"
#include "cfw/test/Check.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Views.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

class Scene : public TreeModel {
public:
    std::size_t childCount(Id parent) const override { return parent == kRoot ? 1 : parent == 1 ? 1 : 0; }
    Id child(Id parent, std::size_t) const override { return parent == kRoot ? 1 : 2; }
    String text(Id node) const override { return node == 1 ? "Workspace" : "Baseplate"; }
};

bool have(const char *command) { return std::system((String(command) + " >/dev/null 2>&1").c_str()) == 0; }

const char *kRegistry = "/usr/libexec/at-spi2-registryd";

// A Python that has pyatspi (the distribution's; another python3 may come
// first on PATH).
String pythonWithPyatspi() {
    for (const char *python : {"python3", "/usr/bin/python3", "/usr/bin/python3.13", "/usr/bin/python3.12",
                               "/usr/bin/python3.11", "/usr/bin/python3.10"}) {
        if (have((String(python) + " -c 'import pyatspi'").c_str())) {
            return python;
        }
    }
    return {};
}

pid_t spawn(std::vector<String> argv, int *stdoutPipe = nullptr) {
    int out[2] = {-1, -1};
    if (stdoutPipe && ::pipe(out) != 0) {
        return -1;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        if (stdoutPipe) {
            dup2(out[1], 1);
            ::close(out[0]);
        }
        std::vector<char *> args;
        for (String &a : argv) {
            args.push_back(a.data());
        }
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    if (stdoutPipe) {
        ::close(out[1]);
        *stdoutPipe = out[0];
    }
    return pid;
}

} // namespace

int main() {
    const String python = pythonWithPyatspi();
    if (!std::getenv("DISPLAY") || !have("command -v dbus-daemon") || access(kRegistry, X_OK) != 0 ||
        python.empty()) {
        std::printf("AtspiTest: skipped (needs a display, dbus-daemon, at-spi2-registryd and pyatspi)\n");
        return cfw::test::finish("AtspiTest");
    }

    // A private bus serves as both the session and the accessibility bus.
    int daemonOut = -1;
    const pid_t daemon = spawn({"dbus-daemon", "--session", "--nofork", "--print-address"}, &daemonOut);
    std::string address;
    char c = 0;
    while (::read(daemonOut, &c, 1) == 1 && c != '\n') {
        address += c;
    }
    setenv("DBUS_SESSION_BUS_ADDRESS", address.c_str(), 1);
    setenv("AT_SPI_BUS_ADDRESS", address.c_str(), 1);
    const pid_t registry = spawn({kRegistry});
    // Wait for the registry's name.
    bool registryUp = false;
    if (auto bus = dbus::Connection::open(address, std::chrono::seconds(2))) {
        for (int i = 0; i < 100 && !registryUp; ++i) {
            auto owned = bus.value()->call(
                dbus::Message::methodCall("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                          "NameHasOwner", {dbus::Value::string("org.a11y.atspi.Registry")}),
                std::chrono::seconds(1));
            registryUp = owned && !owned.value().body.empty() && owned.value().body[0].asBool();
            if (!registryUp) {
                usleep(50000);
            }
        }
    }
    check(registryUp, "the AT-SPI registry is on the private bus");

    auto created = UiWindow::create({"AtspiTest", {420, 260}, false, true}, Theme::dark().withSystemFonts());
    check(created.ok(), "a window");
    int clicks = 0;
    std::map<String, String> found;
    if (created.ok() && registryUp) {
        std::unique_ptr<UiWindow> ui = std::move(created).value();
        ui->surface().accessibleTitle = "AtspiTest";
        auto &column = static_cast<Stack &>(ui->surface().root().add(std::make_unique<Stack>(Stack::Direction::Column, 6.0f, 6.0f)));
        auto &row = column.add<Stack>(Stack::Direction::Row, 6.0f);
        Button &save = row.add<Button>("Save");
        static_cast<void>(save.clicked.connect([&] { ++clicks; }));
        row.add<CheckBox>("Snap", true);
        TextField &name = row.add<TextField>("Part");
        name.setAccessibleName("Name");
        NumberField &size = row.add<NumberField>(4.0, 1);
        size.setRange(0.0, 10.0);
        size.setAccessibleName("Size");
        Scene scene;
        TreeView &tree = column.add<TreeView>(scene);
        tree.setAccessibleName("Explorer");
        tree.setFixedSize({0, 100});
        for (int i = 0; i < 5; ++i) {
            processEvents(std::chrono::milliseconds(10));
            ui->frame();
        }

        int oracleOut = -1;
        const pid_t oracle = spawn({python, CFW_ATSPI_ORACLE, "AtspiTest"}, &oracleOut);
        fcntl(oracleOut, F_SETFL, O_NONBLOCK);
        std::vector<String> lines;
        String partial;
        bool focusSent = false;
        std::chrono::steady_clock::time_point listeningAt{};
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(40);
        int status = -1;
        while (std::chrono::steady_clock::now() < end) {
            processEvents(std::chrono::milliseconds(5));
            ui->frame();
            char buffer[4096];
            ssize_t n = 0;
            while ((n = ::read(oracleOut, buffer, sizeof buffer)) > 0) {
                partial.append(buffer, std::size_t(n));
                for (std::size_t nl; (nl = partial.find('\n')) != String::npos;) {
                    lines.push_back(partial.substr(0, nl));
                    if (lines.back() == "LISTENING") {
                        listeningAt = std::chrono::steady_clock::now();
                    }
                    partial.erase(0, nl + 1);
                }
            }
            // Once the oracle listens (and the registry knows), move the focus.
            if (!focusSent && listeningAt != std::chrono::steady_clock::time_point{} &&
                std::chrono::steady_clock::now() - listeningAt > std::chrono::milliseconds(500)) {
                ui->surface().setFocus(&name);
                focusSent = true;
            }
            if (waitpid(oracle, &status, WNOHANG) == oracle) {
                // Drain what is left.
                while ((n = ::read(oracleOut, buffer, sizeof buffer)) > 0) {
                    partial.append(buffer, std::size_t(n));
                }
                for (std::size_t nl; (nl = partial.find('\n')) != String::npos;) {
                    lines.push_back(partial.substr(0, nl));
                    partial.erase(0, nl + 1);
                }
                break;
            }
        }
        if (status == -1) {
            kill(oracle, SIGKILL);
            waitpid(oracle, nullptr, 0);
        }
        String all;
        for (const String &line : lines) {
            all += line + "\n";
            const std::size_t space = line.find(' ');
            if (space != String::npos && line.substr(0, 4) != "NODE") {
                found[line.substr(0, space)] = line.substr(space + 1);
            }
        }
        const auto has = [&](const char *text) { return all.find(text) != String::npos; };
        check(has("APP 'AtspiTest' 1"), "pyatspi finds the application, with one window");
        check(has("FRAME frame 'AtspiTest' 0"), "the window is a frame");
        check(has("NODE 1 push button 'Save'"), "the button, by role and name");
        check(has("NODE 1 check box 'Snap' focusable,checked"), "the check box, checked");
        check(has("NODE 1 text 'Name' focusable,editable"), "the text field, editable");
        check(has("'Part'"), "its text through the Text interface");
        check(has("NODE 1 spin button 'Size'"), "the number field");
        check(has("NODE 1 tree 'Explorer'") && has("NODE 2 tree item 'Workspace'"), "the tree and its row");
        checkEqual(found["ACTION"], String("1 click True"), "the button's action is click, and it runs");
        checkEqual(clicks, 1, "the button was pressed");
        check(found["SETTEXT"] == "True" && name.text() == "Wall", "setting text contents");
        checkEqual(found["TEXTNOW"], String("'Wall'"), "and reading it back");
        checkEqual(found["VALUE"], String("4.0 0.0 10.0"), "the number field's value and range");
        checkEqual(size.value(), 7.0, "setting the value");
        check(found["EXPAND"] == "expand or contract True" && found["CHILD"] == "'Baseplate'",
              "expanding a tree item shows its child");
        {
            int x = 0, y = 0, w = 0, h = 0;
            std::sscanf(found["EXTENTS"].c_str(), "%d %d %d %d", &x, &y, &w, &h);
            const Vec2i origin = ui->window().screenPosition();
            check(w > 0 && h > 0 && x >= origin.x && y >= origin.y, "extents on the screen");
        }
        checkEqual(found["FOCUS"], String("'Name'"), "a focus change is an event the reader hears");
        if (cfw::test::gFailures > 0) {
            std::printf("oracle said:\n%s", all.c_str());
        }
    }
    kill(registry, SIGTERM);
    waitpid(registry, nullptr, 0);
    kill(daemon, SIGTERM);
    waitpid(daemon, nullptr, 0);
    return cfw::test::finish("AtspiTest");
}
