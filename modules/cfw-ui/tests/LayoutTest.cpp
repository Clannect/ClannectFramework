// Box and Grid layout, hover-within, input-transparent overlays,
// focus-visible, ToggleSwitch, SegmentedControl, ToastHost and the Label
// typography options, headless.

#include <chrono>
#include <cstdio>

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/test/Check.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Layout.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Switches.h"
#include "cfw/ui/Toast.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

PointerEvent pointer(PointerEvent::Type type, Vec2 at, PointerButton button = PointerButton::Left) {
    PointerEvent e;
    e.type = type;
    e.position = at;
    e.button = type == PointerEvent::Type::Move ? PointerButton::None : button;
    return e;
}

void click(Surface &surface, Vec2 at, PointerButton button = PointerButton::Left) {
    surface.dispatch(pointer(PointerEvent::Type::Move, at));
    surface.dispatch(pointer(PointerEvent::Type::Press, at, button));
    surface.dispatch(pointer(PointerEvent::Type::Release, at, button));
}

KeyEvent key(Key k) {
    KeyEvent e;
    e.key = k;
    return e;
}

Image paint(Surface &surface) {
    Image image = Image::create(std::uint32_t(surface.size().x), std::uint32_t(surface.size().y),
                                AlphaMode::Premultiplied)
                      .value();
    RasterPaintBackend backend(image);
    Painter painter(backend);
    surface.paint(painter);
    return image;
}

// A fixed-size block, for layout arithmetic without fonts.
class Block : public Element {
public:
    explicit Block(Vec2 size) { setFixedSize(size); }
};

template <class T> T &as(Element &e) { return static_cast<T &>(e); }

void rows() {
    Surface surface;
    surface.setSize({300, 100});
    auto &row = surface.root().add<Box>(Box::Direction::Row, 10.0f);
    row.setPadding(Insets{5, 4, 5, 4});
    auto &a = row.add<Block>(Vec2{40, 20});
    auto &b = row.add<Block>(Vec2{0, 20});
    b.setStretch(1);
    auto &c = row.add<Block>(Vec2{60, 30});
    surface.layout();
    checkNear(a.rect().x, 5, 0.01, "padding on the left");
    checkNear(a.rect().width, 40, 0.01, "a fixed child keeps its width");
    checkNear(b.rect().x, 55, 0.01, "the gap follows it");
    checkNear(b.rect().width, 300 - 10 - 40 - 60 - 20, 0.01, "the stretching child takes the spare width");
    checkNear(c.rect().x + c.rect().width, 295, 0.01, "the last child ends at the right padding");
    checkNear(a.rect().height, 92, 0.01, "Stretch alignment fills the cross axis inside the padding");

    row.setAlign(Box::Align::Center);
    surface.layout();
    checkNear(a.rect().height, 20, 0.01, "centred children keep their height");
    checkNear(a.rect().y, 4 + (92 - 20) / 2, 0.51, "and are centred");
}

void justifyAndShrink() {
    Surface surface;
    surface.setSize({200, 40});
    auto &row = surface.root().add<Box>(Box::Direction::Row, 0.0f);
    row.setJustify(Box::Justify::SpaceBetween);
    auto &a = row.add<Block>(Vec2{30, 10});
    auto &b = row.add<Block>(Vec2{30, 10});
    auto &c = row.add<Block>(Vec2{30, 10});
    surface.layout();
    checkNear(a.rect().x, 0, 0.01, "space-between: first at the start");
    checkNear(c.rect().x, 170, 0.01, "last at the end");
    checkNear(b.rect().x, 85, 0.51, "the middle one in the middle");

    row.setJustify(Box::Justify::End);
    surface.layout();
    checkNear(a.rect().x, 110, 0.01, "end justification");

    // Too wide: the stretching child shrinks, the fixed ones keep their size.
    Surface narrow;
    narrow.setSize({100, 40});
    auto &r2 = narrow.root().add<Box>(Box::Direction::Row, 0.0f);
    auto &label = r2.add<Block>(Vec2{0, 10});
    label.setStretch(1);
    // A block that would like to be 150 wide (fixed size 0 means "measure").
    class Wide : public Element {
    protected:
        Vec2 measureContent(Vec2) override { return {150, 10}; }
    };
    auto &wide = r2.add<Wide>();
    wide.setStretch(1);
    auto &fixed = r2.add<Block>(Vec2{40, 10});
    narrow.layout();
    checkNear(fixed.rect().width, 40, 0.01, "a fixed child is not shrunk");
    checkNear(label.rect().width + wide.rect().width, 60, 0.01, "stretching children give up the overflow");
    check(wide.rect().width >= 0.0f && label.rect().width >= 0.0f, "nothing below zero");
}

void columnsAndWrap() {
    Surface surface;
    surface.setSize({100, 300});
    auto &col = surface.root().add<Box>(Box::Direction::Column, 6.0f);
    col.setAlign(Box::Align::Start);
    auto &a = col.add<Block>(Vec2{50, 20});
    auto &b = col.add<Block>(Vec2{70, 30});
    surface.layout();
    checkNear(b.rect().y, 26, 0.01, "column gap");
    checkNear(a.rect().width, 50, 0.01, "start alignment keeps widths");
    checkEqual(col.measure({100, 300}).y, 56.0f, "a column measures its children and gaps");

    Surface wrapSurface;
    wrapSurface.setSize({100, 200});
    auto &wrap = wrapSurface.root().add<Box>(Box::Direction::Row, 10.0f);
    wrap.setWrap(true);
    wrap.setLineGap(4.0f);
    auto &w1 = wrap.add<Block>(Vec2{40, 10});
    auto &w2 = wrap.add<Block>(Vec2{40, 10});
    auto &w3 = wrap.add<Block>(Vec2{40, 12});
    wrapSurface.layout();
    checkNear(w2.rect().x, 50, 0.01, "two fit on the first line");
    checkNear(w3.rect().x, 0, 0.01, "the third wraps to the start");
    checkNear(w3.rect().y, 14, 0.01, "below the first line and the line gap");
    checkNear(wrap.measure({100, 200}).y, 26, 0.01, "a wrapped row measures every line");
    (void)w1;

    Surface page;
    page.setSize({1000, 100});
    auto &p = page.root().add<Box>(Box::Direction::Column);
    p.setMaxContentWidth(600);
    auto &content = p.add<Block>(Vec2{0, 10});
    page.layout();
    checkNear(content.rect().x, 200, 0.01, "max content width centres the content");
    checkNear(content.rect().width, 600, 0.01, "at that width");
}

void grids() {
    Surface surface;
    surface.setSize({320, 400});
    auto &grid = surface.root().add<Grid>(3, 10.0f);
    std::vector<Element *> cells;
    for (int i = 0; i < 5; ++i) {
        cells.push_back(&grid.add<Block>(Vec2{0, float(10 + i * 5)}));
    }
    surface.layout();
    checkNear(cells[0]->rect().width, 100, 0.51, "three equal columns");
    checkNear(cells[1]->rect().x, 110, 0.51, "with the gap between");
    checkNear(cells[3]->rect().y, 20 + 10, 0.01, "the second row starts below the tallest of the first");
    checkNear(cells[0]->rect().height, 20, 0.01, "cells fill their row's height");

    grid.setColumnWeights({2, 1});
    surface.layout();
    checkNear(cells[0]->rect().width, 206.67, 0.51, "weights 2:1");
    checkNear(cells[1]->rect().width, 103.33, 0.51, "share the width");

    grid.setMinColumnWidth(140);
    checkEqual(grid.columnsFor(320), std::size_t(2), "auto-fill: as many as fit");
    checkEqual(grid.columnsFor(100), std::size_t(1), "at least one");
}

void clickableBoxes() {
    Surface surface;
    surface.setSize({200, 100});
    auto &holder = surface.root().add<Box>(Box::Direction::Row);
    holder.setAlign(Box::Align::Start);
    auto &card = holder.add<Box>(Box::Direction::Row);
    card.setFixedSize({100, 50});
    card.setClickable(true);
    card.setHoverBackground(Color{1, 0, 0, 1});
    auto &inner = card.add<Block>(Vec2{40, 40});
    int clicks = 0;
    int doubles = 0;
    int menus = 0;
    ScopedConnection c1 = card.clicked.connect([&] { ++clicks; });
    ScopedConnection c2 = card.doubleClicked.connect([&] { ++doubles; });
    ScopedConnection c3 = card.contextMenuRequested.connect([&](Vec2) { ++menus; });
    surface.layout();

    surface.dispatch(pointer(PointerEvent::Type::Move, {10, 10}));
    check(surface.hovered() == &inner, "the child is the hovered element");
    check(card.isHoverWithin(), "the card is hovered within");
    check(!surface.takeDamage().isEmpty(), "hover within repaints a card with a hover colour");

    click(surface, {10, 10});
    checkEqual(clicks, 1, "a click on the card's child clicks the card");
    check(card.hasFocus() && !surface.focusVisible(), "focus from a click is not focus-visible");

    PointerEvent second = pointer(PointerEvent::Type::Press, {10, 10});
    second.clickCount = 2;
    surface.dispatch(second);
    surface.dispatch(pointer(PointerEvent::Type::Release, {10, 10}));
    checkEqual(doubles, 1, "a double-click press");
    checkEqual(clicks, 2, "and its release clicks too (as in a browser)");

    click(surface, {10, 10}, PointerButton::Right);
    checkEqual(menus, 1, "a right press asks for a context menu");

    surface.dispatch(pointer(PointerEvent::Type::Move, {150, 80}));
    check(!card.isHoverWithin(), "leaving clears hover within");

    surface.focusNext();
    check(surface.focusVisible(), "Tab makes focus visible");
    surface.dispatch(key(Key::Enter));
    checkEqual(clicks, 3, "Enter clicks a focused card");

    // Removing the hovered child clears its ancestors' hover within.
    surface.dispatch(pointer(PointerEvent::Type::Move, {10, 10}));
    check(card.isHoverWithin(), "hovered again");
    card.remove(inner);
    check(!card.isHoverWithin(), "the hovered child went: so did hover within");
    static_cast<void>(paint(surface));
}

void transparentOverlay() {
    Surface surface;
    surface.setSize({100, 100});
    auto &under = surface.root().add<Box>();
    under.setClickable(true);
    int clicks = 0;
    ScopedConnection c4 = under.clicked.connect([&] { ++clicks; });
    auto &overlay = surface.root().add<ToastHost>();
    surface.layout();
    check(overlay.isTransparentForInput(), "a toast host lets the pointer through");
    click(surface, {50, 50});
    checkEqual(clicks, 1, "what is under it gets the click");
}

void toggles() {
    TimePoint time{std::chrono::seconds(10)};
    Surface surface;
    surface.clock = [&time] { return time; };
    surface.setSize({100, 40});
    auto &toggle = surface.root().add<ToggleSwitch>(false, "Instant replay");
    bool last = false;
    int changes = 0;
    ScopedConnection c5 = toggle.toggled.connect([&](bool on) {
        last = on;
        ++changes;
    });
    surface.layout();
    click(surface, {10, 10});
    check(toggle.isChecked() && last && changes == 1, "a click turns it on");
    checkNear(toggle.knobPosition(), 0.0, 1e-6, "the knob starts where it was");
    time += std::chrono::milliseconds(75);
    surface.runTimers();
    check(toggle.knobPosition() > 0.2f && toggle.knobPosition() < 1.0f, "half-way, it is moving");
    time += std::chrono::milliseconds(200);
    surface.runTimers();
    checkNear(toggle.knobPosition(), 1.0, 1e-6, "and it arrives");
    check(!surface.nextTimer().has_value(), "the animation stops its timer");

    toggle.setChecked(false);
    check(!toggle.isChecked() && changes == 1, "setChecked does not emit");

    Theme calm = Theme::dark();
    calm.reducedMotion = true;
    surface.setTheme(calm);
    surface.dispatch(key(Key::Space)); // still focused from the click
    checkNear(toggle.knobPosition(), 1.0, 1e-6, "reduced motion: no animation");
    check(!surface.nextTimer().has_value(), "and no timer");

    AccessibleNode node;
    toggle.describeAccessible(node);
    check(node.states.checkable && node.states.checked, "a checkable, checked switch to a screen reader");
    static_cast<void>(paint(surface));
}

void segments() {
    Surface surface(Theme::dark().withSystemFonts());
    surface.setSize({400, 40});
    auto &seg = surface.root().add<SegmentedControl>(std::vector<String>{"16:9", "9:16", "1:1", "4:5"}, 0);
    seg.setFill(true);
    int chosen = -1;
    ScopedConnection c6 = seg.currentChanged.connect([&](int i) { chosen = i; });
    surface.layout();
    checkNear(seg.segmentRect(1).width, seg.segmentRect(2).width, 1.01, "fill: equal segments");
    const RectF third = seg.segmentRect(2);
    click(surface, {third.x + third.width / 2, third.y + third.height / 2});
    checkEqual(chosen, 2, "a click chooses its segment");
    surface.dispatch(key(Key::Right));
    checkEqual(seg.currentIndex(), 3, "Right moves on");
    surface.dispatch(key(Key::Right));
    checkEqual(seg.currentIndex(), 3, "and stops at the end");
    surface.dispatch(key(Key::Home));
    checkEqual(seg.currentIndex(), 0, "Home goes to the first");
    std::vector<AccessibleNode> items;
    seg.accessibleItems(items);
    check(items.size() == 4 && items[0].states.selected && !items[1].states.selected, "segments are selectable items");
    static_cast<void>(paint(surface));
}

void toasts() {
    TimePoint time{std::chrono::seconds(10)};
    Surface surface(Theme::dark().withSystemFonts());
    surface.clock = [&time] { return time; };
    surface.setSize({800, 600});
    auto &host = surface.root().add<ToastHost>();
    surface.layout();
    host.show("Clip saved", {}, std::chrono::seconds(1));
    checkEqual(host.count(), std::size_t(1), "one toast");
    static_cast<void>(paint(surface));
    for (int i = 0; i < 6; ++i) {
        host.show("Toast " + std::to_string(i), {}, std::chrono::seconds(5));
    }
    time += std::chrono::milliseconds(400);
    surface.runTimers();
    check(host.count() <= ToastHost::kMaxVisible + 1, "too many: the oldest leave early");
    time += std::chrono::seconds(6);
    surface.runTimers();
    checkEqual(host.count(), std::size_t(0), "all gone after their time and the fade");
    check(!surface.nextTimer().has_value(), "the host stops its timer when empty");
}

void labels() {
    Surface surface(Theme::dark().withSystemFonts());
    if (!surface.theme().font) {
        std::printf("LayoutTest: no system font, label checks skipped\n");
        return;
    }
    surface.setSize({60, 40});
    auto &row = surface.root().add<Box>(Box::Direction::Row);
    auto &label = row.add<Label>("A rather long recording title");
    label.setElide(true);
    label.setStretch(1);
    surface.layout();
    check(label.rect().width <= 60.0f, "an eliding label stays inside its row");
    check(label.measure({1000, 40}).x > 60.0f, "and still knows its natural width");
    label.setColor(Color{1, 0, 0, 1});
    label.setFont(surface.theme().monoFont ? surface.theme().monoFont : surface.theme().font, 18.0f);
    check(label.measure({1000, 40}).y >= 18.0f, "a font of its own sets the height");
    static_cast<void>(paint(surface));
}

} // namespace

void inputOnlyInChildren() {
    Surface surface;
    surface.setSize({200, 100});
    auto &under = surface.root().add<Box>();
    under.setClickable(true);
    int underClicks = 0;
    ScopedConnection a = under.clicked.connect([&] { ++underClicks; });
    auto &layer = surface.root().add<Box>(Box::Direction::Row);
    layer.setInputOnlyInChildren(true);
    layer.setJustify(Box::Justify::End);
    auto &panel = layer.add<Box>();
    panel.setFixedSize({50, 100});
    panel.setClickable(true);
    int panelClicks = 0;
    ScopedConnection b = panel.clicked.connect([&] { ++panelClicks; });
    surface.layout();
    click(surface, {20, 50});
    checkEqual(underClicks, 1, "a press between an overlay's children reaches what is under it");
    click(surface, {180, 50});
    checkEqual(panelClicks, 1, "the overlay's children take presses");
    checkEqual(underClicks, 1, "and keep them");
}

int main() {
    rows();
    justifyAndShrink();
    columnsAndWrap();
    grids();
    clickableBoxes();
    transparentOverlay();
    toggles();
    segments();
    toasts();
    labels();
    inputOnlyInChildren();
    return cfw::test::finish("LayoutTest");
}
