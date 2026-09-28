// A real input method on X11: uim-xim (the uim XIM bridge) with its
// Japanese engine. Keys typed into the window go through the input method;
// "nihon" composes as にほん, shown through the on-the-spot callbacks as
// compositions, and Enter commits it as text. Turning text input off
// (no text input area) lets keys through as keys.
// Skips (and passes) without a display or without uim-xim installed.

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "cfw/platform/Window.h"
#include "cfw/test/Check.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

bool haveProgram(const char *name) {
    const std::string command = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

// Presses and releases a key in `target`, as the X server would deliver it.
void typeKey(Display *display, ::Window target, KeySym sym, unsigned state = 0) {
    XKeyEvent key{};
    key.display = display;
    key.window = target;
    key.root = DefaultRootWindow(display);
    key.subwindow = 0;
    key.time = CurrentTime;
    key.same_screen = True;
    key.keycode = XKeysymToKeycode(display, sym);
    key.state = state;
    key.type = KeyPress;
    XSendEvent(display, target, True, KeyPressMask, reinterpret_cast<XEvent *>(&key));
    key.type = KeyRelease;
    XSendEvent(display, target, True, KeyReleaseMask, reinterpret_cast<XEvent *>(&key));
    XFlush(display);
}

template <class F> bool pumpUntil(F done, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        processEvents(std::chrono::milliseconds(10));
    }
    return true;
}

void pump(int ms) {
    pumpUntil([] { return false; }, ms);
}

} // namespace

int main() {
    Display *probe = XOpenDisplay(nullptr);
    if (!probe || !haveProgram("uim-xim")) {
        std::printf("X11ImeTest: skipped (%s)\n", probe ? "uim-xim is not installed" : "no display");
        if (probe) {
            XCloseDisplay(probe);
        }
        return cfw::test::finish("X11ImeTest");
    }

    // The input method server, with Japanese on by default.
    const pid_t server = fork();
    if (server == 0) {
        setenv("LC_ALL", "C.UTF-8", 1);
        execlp("uim-xim", "uim-xim", "--engine=anthy-utf8", static_cast<char *>(nullptr));
        _exit(127);
    }
    // Wait until it registers as an XIM server.
    const Atom serverAtom = XInternAtom(probe, "@server=uim", False);
    for (int i = 0; i < 200 && XGetSelectionOwner(probe, serverAtom) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    const bool running = XGetSelectionOwner(probe, serverAtom) != 0;
    check(running, "uim-xim is running");

    setenv("XMODIFIERS", "@im=uim", 1);
    auto created = cfw::Window::create({"IME test", {300, 80}, false, true});
    check(created.ok(), "a window");
    if (!created || !running) {
        kill(server, SIGTERM);
        waitpid(server, nullptr, 0);
        XCloseDisplay(probe);
        return cfw::test::finish("X11ImeTest");
    }
    std::unique_ptr<cfw::Window> window = std::move(created).value();
    const auto target = ::Window(reinterpret_cast<std::uintptr_t>(window->nativeHandle()));
    std::vector<CompositionEvent> compositions;
    std::vector<String> typed;
    std::vector<Key> keys;
    ScopedConnection onComposition =
        window->composition.connect([&](const CompositionEvent &e) { compositions.push_back(e); });
    ScopedConnection onText = window->text.connect([&](const TextEvent &e) { typed.push_back(e.text); });
    ScopedConnection onKey = window->key.connect([&](const KeyEvent &e) {
        if (e.type == KeyEvent::Type::Press) {
            keys.push_back(e.key);
        }
    });
    pump(200);
    XSetInputFocus(probe, target, RevertToParent, CurrentTime);
    XFlush(probe);
    window->setTextInputArea(RectF{10, 10, 1, 20});
    pump(300);

    // Turn the Japanese engine on (uim's generic on-key is Shift+Space).
    typeKey(probe, target, XK_space, ShiftMask);
    pump(200);
    for (const KeySym sym : {KeySym(XK_n), KeySym(XK_i), KeySym(XK_h), KeySym(XK_o), KeySym(XK_n), KeySym(XK_n)}) {
        typeKey(probe, target, sym);
        pump(30);
    }
    pumpUntil([&] { return !compositions.empty() && compositions.back().text == "にほん"; });
    check(!compositions.empty(), "the input method composes through the callbacks");
    if (!compositions.empty()) {
        checkEqual(compositions.back().text, String("にほん"), "\"nihonn\" composes as にほん");
        checkEqual(compositions.back().cursor, std::size_t(9), "the caret is at its end");
    }
    check(typed.empty(), "nothing is committed while composing");

    typeKey(probe, target, XK_Return);
    pumpUntil([&] { return !typed.empty(); });
    check(typed.size() == 1 && typed[0] == "にほん", "Enter commits the composition as text");
    pumpUntil([&] { return !compositions.empty() && compositions.back().text.empty(); }, 1000);
    check(!compositions.empty() && compositions.back().text.empty(), "and the composition ends");

    // With text input off the input method is out of the way.
    window->setTextInputArea(std::nullopt);
    pump(100);
    typed.clear();
    keys.clear();
    compositions.clear();
    typeKey(probe, target, XK_w);
    pumpUntil([&] { return !keys.empty(); }, 1000);
    check(!keys.empty() && keys.back() == Key::W, "keys arrive as keys");
    check(compositions.empty(), "without composing");

    window.reset();
    kill(server, SIGTERM);
    waitpid(server, nullptr, 0);
    XCloseDisplay(probe);
    return cfw::test::finish("X11ImeTest");
}
