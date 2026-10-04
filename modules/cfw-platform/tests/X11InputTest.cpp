// Input on a real X server (Xvfb in CI), driven as hardware would be through
// the XTEST extension: keys carry their position and their side; relative
// mouse mode reports raw movement while the pointer's position stays put,
// and ends by itself when the focus goes; a confined cursor cannot leave the
// window. Skips (and passes) without a display or without libXtst.

#include <dlfcn.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "cfw/platform/Window.h"
#include "cfw/test/Check.h"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>

using cfw::test::check;
using cfw::test::checkEqual;

namespace {

template <class F> bool pumpUntil(F done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        cfw::processEvents(std::chrono::milliseconds(10));
    }
    return true;
}

// The key code XKB gives the key named `name` (0: none).
unsigned keyCodeNamed(Display *display, const char *name) {
    unsigned found = 0;
    if (XkbDescPtr keyboard = XkbGetMap(display, 0, XkbUseCoreKbd)) {
        if (XkbGetNames(display, XkbKeyNamesMask, keyboard) == Success && keyboard->names && keyboard->names->keys) {
            for (unsigned code = keyboard->min_key_code; code <= keyboard->max_key_code; ++code) {
                if (std::strncmp(keyboard->names->keys[code].name, name, XkbKeyNameLength) == 0) {
                    found = code;
                }
            }
        }
        XkbFreeKeyboard(keyboard, 0, True);
    }
    return found;
}

} // namespace

int main() {
    auto created = cfw::Window::create({"CFW X11 input test", {200, 150}, true, true});
    if (!created) {
        std::printf("X11InputTest: skipped (%s)\n", created.error().message().c_str());
        return cfw::test::finish("X11InputTest");
    }
    std::unique_ptr<cfw::Window> window = std::move(created).value();
    // A second connection plays the user: input injected through XTEST
    // takes the same path through the server as a real keyboard and mouse.
    Display *user = XOpenDisplay(nullptr);
    void *xtst = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
    const auto fakeKey = xtst ? reinterpret_cast<int (*)(Display *, unsigned, Bool, unsigned long)>(dlsym(xtst, "XTestFakeKeyEvent"))
                              : nullptr;
    const auto fakeMotion =
        xtst ? reinterpret_cast<int (*)(Display *, int, int, unsigned long)>(dlsym(xtst, "XTestFakeRelativeMotionEvent"))
             : nullptr;
    const auto fakeButton =
        xtst ? reinterpret_cast<int (*)(Display *, unsigned, Bool, unsigned long)>(dlsym(xtst, "XTestFakeButtonEvent"))
             : nullptr;
    if (!user || !fakeKey || !fakeMotion || !fakeButton) {
        std::printf("X11InputTest: skipped (no libXtst.so.6 to inject input with)\n");
        return cfw::test::finish("X11InputTest");
    }
    const auto id = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(window->nativeHandle()));

    std::vector<cfw::KeyEvent> keys;
    std::vector<cfw::PointerEvent> pointers;
    std::vector<bool> relativeChanges;
    bool focused = false;
    cfw::ScopedConnection c1 = window->key.connect([&](const cfw::KeyEvent &e) { keys.push_back(e); });
    cfw::ScopedConnection c2 = window->pointer.connect([&](const cfw::PointerEvent &e) { pointers.push_back(e); });
    cfw::ScopedConnection c3 = window->relativeMouseChanged.connect([&](bool on) { relativeChanges.push_back(on); });
    cfw::ScopedConnection c4 = window->focusChanged.connect([&](bool f) { focused = f; });

    // Another application's window, to take the focus away with.
    const ::Window other = XCreateSimpleWindow(user, DefaultRootWindow(user), 400, 400, 50, 50, 0, 0, 0);
    XMapWindow(user, other);
    XSync(user, False);

    // No window manager under Xvfb: give the window the focus and the pointer by hand.
    cfw::processEvents(std::chrono::milliseconds(50));
    XSetInputFocus(user, id, RevertToPointerRoot, CurrentTime);
    XWarpPointer(user, 0, id, 0, 0, 0, 0, 60, 40);
    XFlush(user);
    check(pumpUntil([&] { return focused; }), "the window gets the focus");

    // ---- Keys: the layout's key, and the key's position and side ----
    window->setRelativeMouse(true); // refused silently where it cannot work; checked below
    const bool relativeWorks = window->isRelativeMouse();
    window->setRelativeMouse(false);
    relativeChanges.clear();

    struct Expect {
        const char *name;
        cfw::Key key;
        cfw::Key physical;
    };
    for (const Expect &expect : {Expect{"AD02", cfw::Key::W, cfw::Key::W}, Expect{"AC01", cfw::Key::A, cfw::Key::A},
                                 Expect{"LFSH", cfw::Key::Shift, cfw::Key::LeftShift},
                                 Expect{"RTSH", cfw::Key::Shift, cfw::Key::RightShift},
                                 Expect{"LCTL", cfw::Key::Control, cfw::Key::LeftControl},
                                 Expect{"RCTL", cfw::Key::Control, cfw::Key::RightControl},
                                 Expect{"UP", cfw::Key::Up, cfw::Key::Up}}) {
        const unsigned code = keyCodeNamed(user, expect.name);
        if (code == 0) {
            std::printf("X11InputTest: this server has no key named %s\n", expect.name);
            continue;
        }
        keys.clear();
        fakeKey(user, code, True, 0);
        fakeKey(user, code, False, 0);
        XFlush(user);
        check(pumpUntil([&] { return keys.size() >= 2; }), "a key press and release arrive");
        if (keys.size() >= 2) {
            check(keys[0].type == cfw::KeyEvent::Type::Press && keys[1].type == cfw::KeyEvent::Type::Release, "in order");
            checkEqual(keys[0].key, expect.key, "the key the layout gives (a modifier whichever side)");
            checkEqual(keys[0].physicalKey, expect.physical, "the key by position, modifiers by side");
        }
    }

    // ---- Relative mouse mode ----
    if (!relativeWorks) {
        std::printf("X11InputTest: no XInput 2 raw motion on this server; relative mode not checked\n");
    } else {
        pointers.clear();
        fakeMotion(user, 3, 2, 0); // an ordinary move, for comparison
        XFlush(user);
        pumpUntil([&] { return !pointers.empty(); });
        check(!pointers.empty() && pointers.back().delta == cfw::Vec2{}, "outside relative mode a move has no delta");
        const cfw::Vec2 before = pointers.empty() ? cfw::Vec2{} : pointers.back().position;

        window->setRelativeMouse(true);
        check(window->isRelativeMouse(), "relative mode turns on for a focused window");
        check(relativeChanges == std::vector<bool>{true}, "and says so");
        pointers.clear();
        cfw::Vec2 moved;
        const auto sum = [&] {
            moved = {};
            for (const cfw::PointerEvent &e : pointers) {
                moved = moved + e.delta;
            }
            return moved;
        };
        for (int i = 0; i < 4; ++i) {
            fakeMotion(user, 500, -300, 0); // far more than the 200-pixel window: no edge stops it
        }
        XFlush(user);
        check(pumpUntil([&] { return sum() == cfw::Vec2{2000, -1200}; }), "raw movement arrives whole, past the window's size");
        if (!(moved == cfw::Vec2{2000, -1200})) {
            std::printf("      %zu events, total movement %g, %g\n", pointers.size(), double(moved.x), double(moved.y));
        }
        bool still = true;
        for (const cfw::PointerEvent &e : pointers) {
            still = still && e.type == cfw::PointerEvent::Type::Move && e.position == before;
        }
        check(still, "the pointer's position stays where it was when the mode began");
        pointers.clear();
        fakeButton(user, 1, True, 0);
        fakeButton(user, 1, False, 0);
        XFlush(user);
        check(pumpUntil([&] { return pointers.size() >= 2; }), "buttons still arrive");
        check(pointers.size() >= 2 && pointers[0].type == cfw::PointerEvent::Type::Press && pointers[0].position == before,
              "at the held position");

        // The focus goes (to another window): the mode ends by itself and says so.
        XSetInputFocus(user, other, RevertToPointerRoot, CurrentTime);
        XFlush(user);
        check(pumpUntil([&] { return !focused; }), "the window loses the focus");
        check(!window->isRelativeMouse(), "relative mode ends with the focus");
        check(relativeChanges == std::vector<bool>({true, false}), "and says so");
        window->setRelativeMouse(true);
        check(!window->isRelativeMouse(), "an unfocused window cannot take the mouse");
        XSetInputFocus(user, id, RevertToPointerRoot, CurrentTime);
        XFlush(user);
        pumpUntil([&] { return focused; });
        check(!window->isRelativeMouse(), "and the mode does not come back by itself");
        pointers.clear();
        fakeMotion(user, 1, 1, 0);
        XFlush(user);
        pumpUntil([&] { return !pointers.empty(); });
        check(!pointers.empty() && pointers.back().delta == cfw::Vec2{} &&
                  std::abs(pointers.back().position.x - before.x) <= 2 && std::abs(pointers.back().position.y - before.y) <= 2,
              "afterwards the pointer is where it was, and moves normally");
    }

    // ---- A confined cursor ----
    window->setCursorConfined(true);
    check(window->isCursorConfined(), "confinement turns on");
    cfw::processEvents(std::chrono::milliseconds(20));
    for (int i = 0; i < 5; ++i) {
        fakeMotion(user, 400, 400, 0);
    }
    XSync(user, False);
    cfw::processEvents(std::chrono::milliseconds(50));
    ::Window root = 0, child = 0;
    int rootX = 0, rootY = 0, x = 0, y = 0;
    unsigned mask = 0;
    XQueryPointer(user, id, &root, &child, &rootX, &rootY, &x, &y, &mask);
    const cfw::Vec2i size = window->pixelSize();
    check(x >= 0 && y >= 0 && x < size.x && y < size.y, "the pointer cannot be moved out of the window");
    window->setCursorConfined(false);
    check(!window->isCursorConfined(), "and off");
    for (int i = 0; i < 5; ++i) {
        fakeMotion(user, 400, 400, 0);
    }
    XSync(user, False);
    XQueryPointer(user, id, &root, &child, &rootX, &rootY, &x, &y, &mask);
    check(x >= size.x || y >= size.y, "then it can");

    window.reset();
    cfw::processEvents(std::chrono::milliseconds(0));
    XCloseDisplay(user);
    return cfw::test::finish("X11InputTest");
}
