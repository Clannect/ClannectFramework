// Files dropped on a window from another application over XDND: a second X
// client (standing in for a file manager) enters, moves, and drops a
// text/uri-list; the window reports Enter, Move and the Drop with the file
// paths, answers XdndStatus with whether it takes them, and finishes the
// drop. A refused drag gets a refusing status and a Leave.
// Skips (and passes) without a display.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "cfw/platform/Window.h"
#include "cfw/test/Check.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

struct FileManager {
    Display *display = nullptr;
    ::Window window = 0;
    Atom enter = 0, position = 0, status = 0, leave = 0, drop = 0, finished = 0, selection = 0, uriList = 0,
         copy = 0;

    bool open() {
        display = XOpenDisplay(nullptr);
        if (!display) {
            return false;
        }
        window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
        enter = XInternAtom(display, "XdndEnter", False);
        position = XInternAtom(display, "XdndPosition", False);
        status = XInternAtom(display, "XdndStatus", False);
        leave = XInternAtom(display, "XdndLeave", False);
        drop = XInternAtom(display, "XdndDrop", False);
        finished = XInternAtom(display, "XdndFinished", False);
        selection = XInternAtom(display, "XdndSelection", False);
        uriList = XInternAtom(display, "text/uri-list", False);
        copy = XInternAtom(display, "XdndActionCopy", False);
        return true;
    }
    void close() {
        XDestroyWindow(display, window);
        XCloseDisplay(display);
    }

    void send(::Window target, Atom type, long l1, long l2, long l3, long l4) {
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = target;
        event.xclient.message_type = type;
        event.xclient.format = 32;
        event.xclient.data.l[0] = long(window);
        event.xclient.data.l[1] = l1;
        event.xclient.data.l[2] = l2;
        event.xclient.data.l[3] = l3;
        event.xclient.data.l[4] = l4;
        XSendEvent(display, target, False, NoEventMask, &event);
        XFlush(display);
    }

    // Waits for a client message of `type`, serving selection requests for
    // the dragged files meanwhile.
    bool waitFor(Atom type, XClientMessageEvent &out, const std::string &uris) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < end) {
            while (XPending(display)) {
                XEvent event;
                XNextEvent(display, &event);
                if (event.type == ClientMessage && event.xclient.message_type == type) {
                    out = event.xclient;
                    return true;
                }
                if (event.type == SelectionRequest) {
                    const XSelectionRequestEvent &request = event.xselectionrequest;
                    XSelectionEvent reply{};
                    reply.type = SelectionNotify;
                    reply.requestor = request.requestor;
                    reply.selection = request.selection;
                    reply.target = request.target;
                    reply.time = request.time;
                    reply.property = 0;
                    if (request.target == uriList) {
                        XChangeProperty(display, request.requestor, request.property, uriList, 8, PropModeReplace,
                                        reinterpret_cast<const unsigned char *>(uris.data()), int(uris.size()));
                        reply.property = request.property;
                    }
                    XSendEvent(display, request.requestor, False, 0, reinterpret_cast<XEvent *>(&reply));
                    XFlush(display);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    // Drags over `target` at (x, y) in its client area; drops if `thenDrop`.
    // Returns the status's accept bit, and the finish's when dropped.
    std::pair<bool, bool> dragOnto(::Window target, int x, int y, bool thenDrop, const std::string &uris) {
        int rootX = 0, rootY = 0;
        ::Window child = 0;
        XTranslateCoordinates(display, target, DefaultRootWindow(display), x, y, &rootX, &rootY, &child);
        XSetSelectionOwner(display, selection, window, CurrentTime);
        send(target, enter, 5L << 24, long(uriList), 0, 0);
        send(target, position, 0, (long(rootX) << 16) | long(rootY), CurrentTime, long(copy));
        XClientMessageEvent reply{};
        if (!waitFor(status, reply, uris)) {
            return {false, false};
        }
        const bool accepted = (reply.data.l[1] & 1) != 0;
        if (!thenDrop || !accepted) {
            send(target, leave, 0, 0, 0, 0);
            return {accepted, false};
        }
        send(target, drop, 0, CurrentTime, 0, 0);
        if (!waitFor(finished, reply, uris)) {
            return {accepted, false};
        }
        return {accepted, (reply.data.l[1] & 1) != 0};
    }
};

} // namespace

int main() {
    auto created = cfw::Window::create({"Drop test", {200, 120}, false, true});
    if (!created) {
        std::printf("X11DropTest: skipped (%s)\n", created.error().message().c_str());
        return cfw::test::finish("X11DropTest");
    }
    std::unique_ptr<cfw::Window> window = std::move(created).value();
    const auto target = ::Window(reinterpret_cast<std::uintptr_t>(window->nativeHandle()));
    for (int i = 0; i < 20; ++i) {
        processEvents(std::chrono::milliseconds(10)); // mapped
    }

    std::vector<DropEvent> events;
    bool takeFiles = true;
    window->setDropHandler([&](const DropEvent &event) {
        events.push_back(event);
        return takeFiles;
    });

    FileManager files;
    check(files.open(), "a second X connection");

    const std::string uris = "file:///home/me/tree%20bark.png\r\nfile:///home/me/rock.jpg\r\n";
    std::pair<bool, bool> result{false, false};
    std::atomic<bool> done{false};
    std::thread source([&] {
        result = files.dragOnto(target, 40, 30, true, uris);
        done = true;
    });
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done && std::chrono::steady_clock::now() < end) {
        processEvents(std::chrono::milliseconds(5));
    }
    source.join();
    check(result.first, "the window says it takes the files");
    check(result.second, "and finishes the drop as taken");
    check(events.size() == 2 && events[0].type == DropEvent::Type::Enter && events[1].type == DropEvent::Type::Drop,
          "Enter, then Drop");
    if (events.size() == 2) {
        checkEqual(events[0].position.x, 40.0f, "at the pointer (x)");
        checkEqual(events[0].position.y, 30.0f, "at the pointer (y)");
        check(events[1].paths == std::vector<String>{"/home/me/tree bark.png", "/home/me/rock.jpg"},
              "the drop brings the file paths");
    }

    // A refusing window: the source hears no, and Leave ends the drag.
    events.clear();
    takeFiles = false;
    done = false;
    std::thread refused([&] {
        result = files.dragOnto(target, 10, 10, true, uris);
        done = true;
    });
    const auto end2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done && std::chrono::steady_clock::now() < end2) {
        processEvents(std::chrono::milliseconds(5));
    }
    refused.join();
    for (int i = 0; i < 10; ++i) {
        processEvents(std::chrono::milliseconds(5)); // the Leave
    }
    check(!result.first, "a refusing window says no");
    check(!events.empty() && events.back().type == DropEvent::Type::Leave, "and the drag ends with Leave");

    files.close();
    window.reset();
    return cfw::test::finish("X11DropTest");
}
