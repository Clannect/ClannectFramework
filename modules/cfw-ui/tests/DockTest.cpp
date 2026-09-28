// Docks, headless: the arrangement, closing and showing panels, resizing
// areas and panels, moving a panel by its title bar, and saved state.

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/test/Check.h"
#include "cfw/ui/Dock.h"
#include "cfw/ui/Surface.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

PointerEvent pointer(PointerEvent::Type type, Vec2 at) {
    PointerEvent e;
    e.type = type;
    e.position = at;
    e.button = type == PointerEvent::Type::Move ? PointerButton::None : PointerButton::Left;
    return e;
}

void drag(Surface &surface, Vec2 from, Vec2 to) {
    surface.dispatch(pointer(PointerEvent::Type::Move, from));
    surface.dispatch(pointer(PointerEvent::Type::Press, from));
    surface.dispatch(pointer(PointerEvent::Type::Move, (from + to) * 0.5f));
    surface.dispatch(pointer(PointerEvent::Type::Move, to));
    surface.dispatch(pointer(PointerEvent::Type::Release, to));
}

struct Editor {
    Surface surface;
    DockLayout *docks = nullptr;
    Element *viewport = nullptr;
    DockPanel *explorer = nullptr;
    DockPanel *assets = nullptr;
    DockPanel *properties = nullptr;
    DockPanel *output = nullptr;

    Editor() {
        surface.setSize({1200, 800});
        docks = &static_cast<DockLayout &>(surface.root().add(std::make_unique<DockLayout>()));
        viewport = &docks->setCentral<Element>();
        explorer = &docks->addPanel(std::make_unique<DockPanel>("explorer", "Explorer", std::make_unique<Element>()),
                                    DockArea::Left);
        assets = &docks->addPanel(std::make_unique<DockPanel>("assets", "Assets", std::make_unique<Element>()),
                                  DockArea::Left);
        properties = &docks->addPanel(
            std::make_unique<DockPanel>("properties", "Properties", std::make_unique<Element>()), DockArea::Right);
        output = &docks->addPanel(std::make_unique<DockPanel>("output", "Output", std::make_unique<Element>()),
                                  DockArea::Bottom);
        output->setTitleBarVisible(false);
        docks->setAreaSize(DockArea::Left, 300);
        docks->setAreaSize(DockArea::Right, 310);
        docks->setAreaSize(DockArea::Bottom, 230);
        surface.layout();
    }
    void paint() {
        Image image = Image::create(1200, 800, AlphaMode::Premultiplied).value();
        RasterPaintBackend backend(image);
        Painter painter(backend);
        surface.paint(painter);
    }
};

void arrangement() {
    Editor e;
    const float gap = DockLayout::kGap;
    checkEqual(e.output->rect(), RectF{0, 800 - 230, 1200, 230}, "the bottom area spans the width");
    checkEqual(e.explorer->rect().x, 0.0f, "Explorer on the left");
    checkEqual(e.explorer->rect().width, 300.0f, "at the left area's width");
    checkEqual(e.properties->rect().right(), 1200.0f, "Properties on the right");
    checkEqual(e.properties->rect().width, 310.0f, "at the right area's width");
    checkEqual(e.viewport->rect(), RectF{300 + gap, 0, 1200 - 300 - 310 - 2 * gap, 800 - 230 - gap},
               "the viewport takes the middle");
    check(e.assets->rect().y > e.explorer->rect().bottom(), "Assets below Explorer");
    checkEqual(e.explorer->rect().height + e.assets->rect().height + gap, 800 - 230 - gap,
               "the two share the left area");
    check(e.explorer->titleBarHeight() > 0.0f && e.output->titleBarHeight() == 0.0f, "Output has no title bar");
    e.paint();
}

void closingAndShowing() {
    Editor e;
    std::vector<std::pair<String, bool>> changes;
    ScopedConnection c = e.docks->visibilityChanged.connect(
        [&changes](DockPanel &panel, bool visible) { changes.emplace_back(panel.id(), visible); });

    e.docks->setPanelVisible(*e.assets, false);
    e.surface.layout();
    checkEqual(e.explorer->rect().height, 800.0f - 230 - DockLayout::kGap, "Explorer takes the whole left area");
    checkEqual(changes.size(), std::size_t(1), "one change");
    check(changes[0] == std::make_pair(String("assets"), false), "Assets hidden");

    // The close button in Properties' title bar.
    const RectF bar{e.properties->rect().x, e.properties->rect().y, e.properties->rect().width,
                    e.properties->titleBarHeight()};
    const Vec2 close{bar.right() - 4.0f - 10.0f, bar.y + bar.height / 2.0f};
    e.surface.dispatch(pointer(PointerEvent::Type::Move, close));
    e.surface.dispatch(pointer(PointerEvent::Type::Press, close));
    e.surface.dispatch(pointer(PointerEvent::Type::Release, close));
    e.surface.layout();
    check(!e.properties->isVisible(), "the close button hides the panel");
    checkEqual(e.viewport->rect().right(), 1200.0f, "the viewport grows into the empty area");

    e.docks->setPanelVisible(*e.properties, true);
    e.surface.layout();
    checkEqual(e.properties->rect().width, 310.0f, "shown again at its old width");
    checkEqual(changes.size(), std::size_t(3), "closed and shown");
}

void resizing() {
    Editor e;
    const float gapX = e.explorer->rect().right() + DockLayout::kGap / 2.0f;
    e.surface.dispatch(pointer(PointerEvent::Type::Move, {gapX, 100}));
    checkEqual(e.surface.cursor(), Cursor::SizeHorizontal, "a resize cursor over the gap");
    drag(e.surface, {gapX, 100}, {gapX + 40, 100});
    e.surface.layout();
    checkEqual(e.explorer->rect().width, 340.0f, "dragging the gap widens the left area");

    const float gapY = e.output->rect().y - DockLayout::kGap / 2.0f;
    drag(e.surface, {600, gapY}, {600, gapY - 50});
    e.surface.layout();
    checkEqual(e.output->rect().height, 280.0f, "the bottom area grows upwards");

    // Between Explorer and Assets.
    const float between = e.explorer->rect().bottom() + DockLayout::kGap / 2.0f;
    const float explorerBefore = e.explorer->rect().height;
    const float assetsBefore = e.assets->rect().height;
    drag(e.surface, {100, between}, {100, between + 30});
    e.surface.layout();
    checkEqual(e.explorer->rect().height, explorerBefore + 30, "Explorer grows");
    checkEqual(e.assets->rect().height, assetsBefore - 30, "Assets gives the pixels");

    // Minimums hold.
    drag(e.surface, {100, e.explorer->rect().bottom() + 2}, {100, 2000});
    e.surface.layout();
    check(e.assets->rect().height >= e.assets->minimumExtent() - 0.5f, "a panel keeps its minimum");
}

void moving() {
    Editor e;
    const Vec2 title{e.assets->rect().x + 40, e.assets->rect().y + 8};
    e.surface.dispatch(pointer(PointerEvent::Type::Move, title));
    e.surface.dispatch(pointer(PointerEvent::Type::Press, title));
    e.surface.dispatch(pointer(PointerEvent::Type::Move, title + Vec2{10, 0}));
    e.surface.dispatch(pointer(PointerEvent::Type::Move, {1150, 300}));
    checkEqual(e.surface.popupCount(), std::size_t(1), "a drop indicator shows the target");
    e.paint();
    e.surface.dispatch(pointer(PointerEvent::Type::Release, {1150, 300}));
    e.surface.layout();
    checkEqual(e.surface.popupCount(), std::size_t(0), "and goes with the drop");
    check(e.docks->areaOf(*e.assets) == DockArea::Right, "Assets moved to the right");
    check(e.assets->rect().y > e.properties->rect().y, "below Properties");
    checkEqual(e.explorer->rect().height, 800.0f - 230 - DockLayout::kGap, "Explorer alone on the left");

    // Dropping in the middle changes nothing.
    const Vec2 again{e.explorer->rect().x + 40, e.explorer->rect().y + 8};
    drag(e.surface, again, {600, 300});
    check(e.docks->areaOf(*e.explorer) == DockArea::Left, "a drop in the middle leaves it");
}

void state() {
    Editor e;
    e.docks->movePanel(*e.assets, DockArea::Right, 0);
    e.docks->setPanelVisible(*e.output, false);
    e.docks->setAreaSize(DockArea::Left, 250);
    const String saved = e.docks->saveState();

    Editor fresh;
    check(fresh.docks->restoreState(saved), "the state restores");
    fresh.surface.layout();
    check(fresh.docks->areaOf(*fresh.assets) == DockArea::Right, "Assets on the right");
    check(fresh.docks->panels(DockArea::Right).front() == fresh.assets, "above Properties");
    check(!fresh.output->isVisible(), "Output hidden");
    checkEqual(fresh.explorer->rect().width, 250.0f, "the left width");
    checkEqual(fresh.docks->saveState(), saved, "saving again gives the same text");
    check(!fresh.docks->restoreState("L=abc"), "garbage is refused");
    check(fresh.docks->restoreState("gone:L:100:1"), "unknown panels are skipped");
}

} // namespace

int main() {
    arrangement();
    closingAndShowing();
    resizing();
    moving();
    state();
    return cfw::test::finish("DockTest");
}
