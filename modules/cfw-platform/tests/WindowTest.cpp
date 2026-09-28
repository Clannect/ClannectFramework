// A real native window: presenting pixels and reading them back from the
// window system, resizing, repaint requests, waking a waiting event loop from
// another thread, and the clipboard. Skips (and passes) without a display.

#include <chrono>
#include <cstdio>
#include <thread>

#include "cfw/image/Image.h"
#include "cfw/platform/Window.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

// Pumps events until `done` or about two seconds pass.
template <class F> bool pumpUntil(F done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        processEvents(std::chrono::milliseconds(10));
    }
    return true;
}

Image halves(Vec2i size) {
    Image image = Image::create(std::uint32_t(size.x), std::uint32_t(size.y)).value();
    for (int y = 0; y < size.y; ++y) {
        std::uint8_t *row = image.row(std::uint32_t(y)).data();
        for (int x = 0; x < size.x; ++x) {
            const bool left = x < size.x / 2;
            row[x * 4] = left ? 220 : 10;
            row[x * 4 + 1] = left ? 30 : 180;
            row[x * 4 + 2] = 40;
            row[x * 4 + 3] = 255;
        }
    }
    return image;
}

bool near(const std::uint8_t *p, int r, int g, int b) {
    return std::abs(p[0] - r) <= 2 && std::abs(p[1] - g) <= 2 && std::abs(p[2] - b) <= 2;
}

} // namespace

int main() {
    auto created = Window::create({"CFW window test", {160, 120}, true, true});
    if (!created) {
        std::printf("WindowTest: skipped (%s)\n", created.error().message().c_str());
        check(created.error().code() == ErrorCode::Unsupported, "no window system is reported as Unsupported");
        return cfw::test::finish("WindowTest");
    }
    std::unique_ptr<Window> window = std::move(created).value();
    int repaints = 0;
    std::vector<Vec2i> sizes;
    ScopedConnection c1 = window->repaintRequested.connect([&] { ++repaints; });
    ScopedConnection c2 = window->resized.connect([&](Vec2i s) { sizes.push_back(s); });

    check(window->devicePixelRatio() >= 1.0f, "a device pixel ratio");
    check(window->pixelSize().x >= 160 && window->pixelSize().y >= 120, "the client area has the requested size");
    check(pumpUntil([&] { return repaints > 0; }), "a new window asks to be painted");

    const Vec2i size = window->pixelSize();
    window->present(halves(size));
    processEvents(std::chrono::milliseconds(50));
    auto shot = window->capture();
    check(bool(shot), "the window can be captured");
    if (shot) {
        const Image &image = shot.value();
        check(near(image.row(10).data() + 10 * 4, 220, 30, 40), "the left half is what was presented");
        check(near(image.row(10).data() + std::size_t(size.x - 10) * 4, 10, 180, 40), "and the right half");
    }

    window->setSize({220, 90});
    check(pumpUntil([&] { return !sizes.empty() && sizes.back().x >= 220; }), "resizing reports the new size");
    checkEqual(window->pixelSize(), sizes.empty() ? Vec2i{} : sizes.back(), "pixelSize follows");

    repaints = 0;
    window->requestRepaint();
    processEvents(std::chrono::milliseconds(0));
    checkEqual(repaints, 1, "requestRepaint is answered by the next processEvents");

    // wakeUp() ends a long wait early.
    std::thread waker([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        wakeUp();
    });
    const auto before = std::chrono::steady_clock::now();
    processEvents(std::chrono::seconds(5));
    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
    waker.join();
    check(waited < 2.0, "wakeUp() ends the wait");

    setClipboardText("Clannect ✓");
    checkEqual(clipboardText(), String("Clannect ✓"), "the clipboard round-trips UTF-8");

    window->setTitle("Renamed ✓");
    window->setCursor(Cursor::IBeam);
    window->hide();
    window.reset();
    processEvents(std::chrono::milliseconds(0));
    return cfw::test::finish("WindowTest");
}
