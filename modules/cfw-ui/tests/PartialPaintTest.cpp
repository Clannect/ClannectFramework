// Damage-limited repainting: after each change, repainting only the damaged
// rectangles (as UiWindow::frame does) gives the same pixels as repainting
// everything; damage stays small for small changes.

#include <cmath>
#include <functional>
#include <cstdio>
#include <cstdlib>

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/test/Check.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Layout.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Switches.h"
#include "cfw/ui/Timeline.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

void clear(Image &image, int x, int y, int w, int h) {
    for (int yy = y; yy < y + h; ++yy) {
        auto row = image.row(std::uint32_t(yy));
        std::fill(row.data() + std::size_t(x) * 4u, row.data() + std::size_t(x + w) * 4u, std::uint8_t(0));
    }
}

void full(Surface &s, Image &image) {
    clear(image, 0, 0, int(image.width()), int(image.height()));
    RasterPaintBackend backend(image);
    Painter painter(backend);
    s.paint(painter);
}

// What UiWindow::frame does with the damage rectangles.
float partial(Surface &s, Image &image) {
    float area = 0;
    s.layout();
    for (const RectF &d : s.takeDamageRects()) {
        const int x0 = std::max(0, int(std::floor(d.x)) - 1), y0 = std::max(0, int(std::floor(d.y)) - 1);
        const int x1 = std::min(int(image.width()), int(std::ceil(d.x + d.width)) + 1);
        const int y1 = std::min(int(image.height()), int(std::ceil(d.y + d.height)) + 1);
        clear(image, x0, y0, x1 - x0, y1 - y0);
        RasterPaintBackend backend(image);
        Painter painter(backend);
        painter.clipRect({float(x0), float(y0), float(x1 - x0), float(y1 - y0)});
        s.paint(painter, {float(x0), float(y0), float(x1 - x0), float(y1 - y0)});
        area += float((x1 - x0) * (y1 - y0));
    }
    return area;
}

int differing(const Image &a, const Image &b) {
    int n = 0;
    for (std::size_t i = 0; i < a.pixels().size(); ++i) {
        n += std::abs(int(a.pixels()[i]) - int(b.pixels()[i])) > 2;
    }
    return n;
}

} // namespace

int main() {
    Surface s;
    s.setSize({640, 400});
    auto &root = s.root().add<Box>(Box::Direction::Column, 8.0f);
    root.setPadding(12.0f);
    auto &top = root.add<Box>(Box::Direction::Row, 8.0f);
    auto &card = top.add<Box>(Box::Direction::Column, 6.0f);
    card.setPadding(10.0f);
    card.setBackground(Color{0.2f, 0.2f, 0.25f, 1});
    card.setRadius(8.0f);
    card.setFixedSize({200, 90});
    auto &swatch = top.add<Box>();
    swatch.setFixedSize({60, 60});
    swatch.setBackground(Color{0.8f, 0.1f, 0.2f, 1});
    auto &sw = top.add<ToggleSwitch>(false);
    auto &tl = root.add<Timeline>();
    tl.setStretch(1);
    tl.setZoom(10);
    tl.setTracks({{"V1", "Video", Color{0.2f, 0.25f, 0.4f, 1}, 50}, {"A1", "Audio", Color{0.1f, 0.35f, 0.3f, 1}, 40, true}});
    tl.setClips({{1, 0, 0, 10, "one"}, {2, 0, 15, 8, "two"}, {3, 1, 2, 20, "three"}});

    Image a = Image::create(640, 400, AlphaMode::Premultiplied).value();
    Image b = Image::create(640, 400, AlphaMode::Premultiplied).value();
    full(s, a);
    static_cast<void>(s.takeDamageRects());

    struct Step {
        const char *what;
        std::function<void()> change;
        float maxArea; // of 256000
    };
    const Step steps[] = {
        {"a colour change", [&] { swatch.setBackground(Color{0.1f, 0.7f, 0.2f, 1}); }, 12000},
        {"the playhead moving", [&] { tl.setPlayhead(4.0); }, 60000},
        {"the playhead moving again", [&] { tl.setPlayhead(4.5); }, 60000},
        {"a toggle", [&] { sw.setChecked(true); }, 6000},
        {"a resize", [&] { card.setFixedSize({260, 90}); }, 256000},
        {"a hidden element", [&] { swatch.setVisible(false); }, 256000},
        {"clips replaced", [&] { tl.setClips({{1, 0, 3, 10, "moved"}}); }, 256000},
    };
    for (const Step &st : steps) {
        st.change();
        const float area = partial(s, a);
        full(s, b);
        const int diff = differing(a, b);
        char msg[160];
        std::snprintf(msg, sizeof msg, "%s: partial repaint equals full repaint (%d bytes differ)", st.what, diff);
        check(diff == 0, msg);
        std::snprintf(msg, sizeof msg, "%s: repaints %.0f px, at most %.0f", st.what, area, st.maxArea);
        check(area <= st.maxArea, msg);
        static_cast<void>(s.takeDamageRects());
    }

    // Separate changes stay separate rectangles.
    swatch.setVisible(true);
    s.layout();
    static_cast<void>(s.takeDamageRects());
    s.root().invalidatePaint({0, 0, 10, 10});
    s.root().invalidatePaint({600, 380, 10, 10});
    checkEqual(s.takeDamageRects().size(), std::size_t(2), "far apart damage stays two rectangles");
    s.root().invalidatePaint({0, 0, 10, 10});
    s.root().invalidatePaint({20, 0, 10, 10});
    checkEqual(s.takeDamageRects().size(), std::size_t(1), "near damage merges");
    for (int i = 0; i < 20; ++i) {
        s.root().invalidatePaint({float(i * 30), float((i % 2) * 300), 4, 4});
    }
    check(s.takeDamageRects().size() <= Surface::kMaxDamageRects, "and there are never more than the maximum");
    return cfw::test::finish("PartialPaintTest");
}
