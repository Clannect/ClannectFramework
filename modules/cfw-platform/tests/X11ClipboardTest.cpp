// The clipboard between applications on X11: another X client (a second
// connection on its own thread, standing in for a text editor) pastes what
// CFW copied, and CFW pastes what it copied, small and large (INCR).
// Skips (and passes) without a display.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "cfw/platform/Window.h"
#include "cfw/test/Check.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

struct Other {
    Display *display = nullptr;
    ::Window window = 0;
    Atom clipboard = 0, utf8 = 0, targets = 0, incr = 0, property = 0;

    bool open() {
        display = XOpenDisplay(nullptr);
        if (!display) {
            return false;
        }
        window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
        XSelectInput(display, window, PropertyChangeMask);
        clipboard = XInternAtom(display, "CLIPBOARD", False);
        utf8 = XInternAtom(display, "UTF8_STRING", False);
        targets = XInternAtom(display, "TARGETS", False);
        incr = XInternAtom(display, "INCR", False);
        property = XInternAtom(display, "OTHER_PASTE", False);
        return true;
    }
    void close() {
        XDestroyWindow(display, window);
        XCloseDisplay(display);
    }

    // Pastes the clipboard as UTF8_STRING (small transfers).
    std::string paste() {
        XConvertSelection(display, clipboard, utf8, property, window, CurrentTime);
        XFlush(display);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < end) {
            XEvent event;
            if (XCheckTypedWindowEvent(display, window, SelectionNotify, &event)) {
                if (event.xselection.property == 0) {
                    return "<refused>";
                }
                Atom type = 0;
                int format = 0;
                unsigned long items = 0, remaining = 0;
                unsigned char *data = nullptr;
                XGetWindowProperty(display, window, property, 0, 0x7fffffff, True, AnyPropertyType, &type, &format,
                                   &items, &remaining, &data);
                std::string text(reinterpret_cast<const char *>(data), items);
                XFree(data);
                return text;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return "<timeout>";
    }

    // Owns the clipboard with `text` and serves requests until `stop`,
    // sending it in 1000-byte INCR chunks when `incremental`.
    void serve(const std::string &text, bool incremental, const std::atomic<bool> &stop) {
        XSetSelectionOwner(display, clipboard, window, CurrentTime);
        XFlush(display);
        ::Window pendingRequestor = 0;
        Atom pendingProperty = 0;
        std::size_t sent = 0;
        while (!stop) {
            XEvent event;
            if (XPending(display)) {
                XNextEvent(display, &event);
                if (event.type == SelectionRequest) {
                    const XSelectionRequestEvent &request = event.xselectionrequest;
                    XSelectionEvent reply{};
                    reply.type = SelectionNotify;
                    reply.requestor = request.requestor;
                    reply.selection = request.selection;
                    reply.target = request.target;
                    reply.time = request.time;
                    reply.property = request.property;
                    if (request.target == utf8 && incremental) {
                        XSelectInput(display, request.requestor, PropertyChangeMask);
                        const long size = long(text.size());
                        XChangeProperty(display, request.requestor, request.property, incr, 32, PropModeReplace,
                                        reinterpret_cast<const unsigned char *>(&size), 1);
                        pendingRequestor = request.requestor;
                        pendingProperty = request.property;
                        sent = 0;
                    } else if (request.target == utf8) {
                        XChangeProperty(display, request.requestor, request.property, utf8, 8, PropModeReplace,
                                        reinterpret_cast<const unsigned char *>(text.data()), int(text.size()));
                    } else {
                        reply.property = 0;
                    }
                    XSendEvent(display, request.requestor, False, 0, reinterpret_cast<XEvent *>(&reply));
                    XFlush(display);
                } else if (event.type == PropertyNotify && event.xproperty.window == pendingRequestor &&
                           event.xproperty.atom == pendingProperty && event.xproperty.state == PropertyDelete) {
                    // The reader took the last chunk: send the next (empty at the end).
                    const std::size_t chunk = std::min<std::size_t>(1000, text.size() - sent);
                    XChangeProperty(display, pendingRequestor, pendingProperty, utf8, 8, PropModeReplace,
                                    reinterpret_cast<const unsigned char *>(text.data() + sent), int(chunk));
                    XFlush(display);
                    sent += chunk;
                    if (chunk == 0) {
                        pendingRequestor = 0;
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }
};

} // namespace

int main() {
    auto created = cfw::Window::create({"Clipboard test", {64, 64}, false, false});
    if (!created) {
        std::printf("X11ClipboardTest: skipped (%s)\n", created.error().message().c_str());
        return cfw::test::finish("X11ClipboardTest");
    }
    Other other;
    check(other.open(), "a second X connection");

    // Another application pastes what CFW copied (CFW answers from its loop).
    setClipboardText("Clannect ✓ copied");
    std::string pasted;
    std::thread paster([&] { pasted = other.paste(); });
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (pasted.empty() && std::chrono::steady_clock::now() < end) {
        processEvents(std::chrono::milliseconds(5));
    }
    paster.join();
    checkEqual(String(pasted), String("Clannect ✓ copied"), "another application pastes CFW's text");

    // CFW pastes what another application copied.
    {
        std::atomic<bool> stop{false};
        std::thread owner([&] { other.serve("from the other app ✓", false, stop); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        checkEqual(clipboardText(), String("from the other app ✓"), "CFW pastes another application's text");
        stop = true;
        owner.join();
    }

    // Large text arrives in increments.
    {
        std::string big;
        for (int i = 0; i < 5000; ++i) {
            big += "line " + std::to_string(i) + "\n";
        }
        std::atomic<bool> stop{false};
        std::thread owner([&] { other.serve(big, true, stop); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const String got = clipboardText();
        check(got.size() == big.size() && got == big, "an INCR transfer arrives whole");
        stop = true;
        owner.join();
    }

    // Copying again takes the clipboard back.
    setClipboardText("mine again");
    checkEqual(clipboardText(), String("mine again"), "copying takes the clipboard back");

    other.close();
    created.value().reset();
    return cfw::test::finish("X11ClipboardTest");
}
