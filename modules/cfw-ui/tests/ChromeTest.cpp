// Icons, icon and toggle buttons, submenus, the menu bar, toolbars,
// sliders and progress bars, headless.

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/test/Check.h"
#include "cfw/ui/Chrome.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Icon.h"
#include "cfw/ui/Surface.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

PointerEvent pointer(PointerEvent::Type type, float x, float y) {
    PointerEvent e;
    e.type = type;
    e.position = {x, y};
    e.button = type == PointerEvent::Type::Move ? PointerButton::None : PointerButton::Left;
    return e;
}

KeyEvent key(Key k, Modifier modifiers = Modifier::None) {
    KeyEvent e;
    e.key = k;
    e.modifiers = modifiers;
    return e;
}

void click(Surface &surface, Vec2 at) {
    surface.dispatch(pointer(PointerEvent::Type::Move, at.x, at.y));
    surface.dispatch(pointer(PointerEvent::Type::Press, at.x, at.y));
    surface.dispatch(pointer(PointerEvent::Type::Release, at.x, at.y));
}

void move(Surface &surface, Vec2 at) { surface.dispatch(pointer(PointerEvent::Type::Move, at.x, at.y)); }

Image paint(Surface &surface) {
    Image image = Image::create(std::uint32_t(surface.size().x), std::uint32_t(surface.size().y),
                                AlphaMode::Premultiplied)
                      .value();
    RasterPaintBackend backend(image);
    Painter painter(backend);
    surface.paint(painter);
    return image;
}

// A filled square covering the whole 24-unit view box.
constexpr const char *kSquare =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><rect width="24" height="24" fill="currentColor"/></svg>)";

void icons() {
    const Icon square = Icon::fromSvg(kSquare);
    check(!square.isNull(), "an SVG icon parses");
    check(Icon::fromSvg("<html/>").isNull(), "anything else is a null icon");

    Image image = Image::create(8, 8, AlphaMode::Premultiplied).value();
    {
        RasterPaintBackend backend(image);
        Painter painter(backend);
        square.paint(painter, {0, 0, 4, 8}, Color{1, 0, 0, 1});
        square.withColor(Color{0, 0, 1, 1}).paint(painter, {4, 0, 4, 8}, Color{1, 0, 0, 1});
    }
    const std::uint8_t *left = image.pixels().data() + image.stride() * 4 + 4 * 1;
    const std::uint8_t *right = image.pixels().data() + image.stride() * 4 + 4 * 6;
    checkEqual(int(left[0]), 255, "currentColor takes the tint");
    checkEqual(int(right[2]), 255, "an icon's own colour wins");
    checkEqual(int(right[0]), 0, "and ignores the tint");
}

void buttons() {
    Surface surface;
    surface.setSize({300, 100});
    auto &row = static_cast<Stack &>(surface.root().add(std::make_unique<Stack>(Stack::Direction::Row, 0.0f)));
    Button &iconOnly = row.add<Button>(Icon::fromSvg(kSquare));
    Button &toggle = row.add<Button>("Snap");
    toggle.setCheckable(true);
    int toggles = 0;
    bool lastState = false;
    ScopedConnection c = toggle.toggled.connect([&](bool on) {
        ++toggles;
        lastState = on;
    });
    surface.layout();

    const Vec2 wanted = iconOnly.measure({300, 100});
    checkEqual(wanted.x, wanted.y, "an icon-only button wants to be square");
    click(surface, toggle.rect().center());
    check(toggle.isChecked() && lastState && toggles == 1, "a click checks a checkable button");
    surface.setFocus(&toggle);
    surface.dispatch(key(Key::Space));
    check(!toggle.isChecked() && !lastState && toggles == 2, "Space unchecks it");
    toggle.setChecked(true);
    checkEqual(toggles, 2, "setChecked does not emit");
    static_cast<void>(paint(surface)); // every state paints
}

void submenus() {
    Surface surface;
    surface.setSize({600, 400});
    surface.layout();
    int ran = 0;
    auto menu = std::make_unique<Menu>();
    menu->addItem("First", [] {});
    MenuItem &more = menu->addSubmenu("More", [&ran](Menu &sub) {
        sub.addItem("Deep", [&ran] { ++ran; });
        sub.addItem("Deeper", [] {});
    });
    MenuItem &last = menu->addItem("Last", [] {});
    Menu &root = Menu::popup(surface, std::move(menu), {10, 10});
    surface.layout();

    move(surface, more.rect().center());
    checkEqual(surface.popupCount(), std::size_t(2), "hovering a submenu item opens its submenu");
    Menu *sub = root.openSubmenu();
    check(sub != nullptr && sub->parentMenu() == &root, "the submenu knows its parent");
    surface.layout();
    check(sub->rect().x >= root.rect().right() - 5.0f, "beside the menu");
    move(surface, last.rect().center());
    checkEqual(surface.popupCount(), std::size_t(1), "hovering a sibling closes it");

    move(surface, more.rect().center());
    surface.layout();
    Element *deep = root.openSubmenu()->children()[0].get();
    click(surface, deep->rect().center());
    checkEqual(ran, 1, "an item in a submenu runs");
    checkEqual(surface.popupCount(), std::size_t(0), "and closes the whole chain");

    // The keyboard: Right opens and enters, Left leaves.
    auto again = std::make_unique<Menu>();
    again->addSubmenu("More", [](Menu &s) { s.addItem("Deep", [] {}); });
    Menu &keyboard = Menu::popup(surface, std::move(again), {10, 10});
    surface.layout();
    keyboard.focusFirstItem();
    Element *moreItem = surface.focus();
    surface.dispatch(key(Key::Right));
    checkEqual(surface.popupCount(), std::size_t(2), "Right opens the submenu");
    check(surface.focus() && surface.focus()->parent() == keyboard.openSubmenu(), "and focuses its first item");
    surface.dispatch(key(Key::Left));
    checkEqual(surface.popupCount(), std::size_t(1), "Left closes it");
    check(surface.focus() == moreItem, "and goes back to its item");
    surface.closePopups();

    // Closing a popup closes what was opened above it.
    Element &bottom = surface.openPopup(std::make_unique<Menu>(), {0, 0});
    surface.openPopup(std::make_unique<Menu>(), {50, 0});
    surface.openPopup(std::make_unique<Menu>(), {100, 0});
    surface.closePopup(bottom);
    checkEqual(surface.popupCount(), std::size_t(0), "popups are a stack");
}

void menuBar() {
    Surface surface;
    surface.setSize({600, 400});
    auto &column = static_cast<Stack &>(surface.root().add(std::make_unique<Stack>(Stack::Direction::Column, 0.0f)));
    MenuBar &bar = column.add<MenuBar>();
    int saved = 0;
    bar.addMenu("&File", [&saved](Menu &m) { m.addItem("Save", [&saved] { ++saved; }, "Ctrl+S"); });
    bar.addMenu("&Edit", [](Menu &m) { m.addItem("Undo", [] {}); });
    bar.addMenu("&View", [](Menu &m) { m.addItem("Output", [] {}); });
    column.add<Element>().setStretch(1);
    surface.layout();
    static_cast<void>(paint(surface)); // lays the titles out

    const RectF barRect = bar.rect();
    const float y = barRect.y + barRect.height / 2.0f;
    // The titles are laid out left to right from x = 4; find them by probing.
    std::vector<float> titleX;
    std::ptrdiff_t last = -1;
    for (float x = barRect.x; x < barRect.right(); x += 1.0f) {
        click(surface, {x, y});
        if (bar.openIndex() != last && bar.openIndex() >= 0 && std::size_t(bar.openIndex()) == titleX.size()) {
            titleX.push_back(x);
        }
        last = bar.openIndex();
        surface.closePopups();
        last = -1;
        if (titleX.size() == 3) {
            break;
        }
    }
    checkEqual(titleX.size(), std::size_t(3), "each title opens its menu");

    click(surface, {titleX[0] + 2, y});
    checkEqual(bar.openIndex(), std::ptrdiff_t(0), "File is open");
    move(surface, {titleX[1] + 2, y});
    checkEqual(bar.openIndex(), std::ptrdiff_t(1), "moving over Edit switches to it");
    checkEqual(surface.popupCount(), std::size_t(1), "one menu at a time");

    bar.openMenu(0, true);
    surface.dispatch(key(Key::Right));
    checkEqual(bar.openIndex(), std::ptrdiff_t(1), "Right moves to the next menu");
    surface.dispatch(key(Key::Left));
    surface.dispatch(key(Key::Left));
    checkEqual(bar.openIndex(), std::ptrdiff_t(2), "Left wraps around");
    surface.dispatch(key(Key::Escape));
    checkEqual(bar.openIndex(), std::ptrdiff_t(-1), "Escape closes it");

    bar.openMenu(0, true);
    surface.dispatch(key(Key::Enter));
    checkEqual(saved, 1, "Enter runs the focused item");
    checkEqual(bar.openIndex(), std::ptrdiff_t(-1), "and closes the menu");
}

void toolBar() {
    Surface surface;
    surface.setSize({400, 100});
    ToolBar &bar = static_cast<ToolBar &>(surface.root().add(std::make_unique<ToolBar>()));
    int pressed = 0;
    bool snapped = false;
    Button &save = bar.addButton(Icon::fromSvg(kSquare), "Save", [&pressed] { ++pressed; });
    bar.addSeparator();
    Button &snap = bar.addToggle(Icon::fromSvg(kSquare), "Snap", [&snapped](bool on) { snapped = on; });
    surface.layout();

    checkEqual(save.toolTip(), String("Save"), "a toolbar button's tooltip");
    check(!save.isFocusable(), "toolbar buttons do not take the keyboard");
    click(surface, save.rect().center());
    checkEqual(pressed, 1, "a toolbar button runs its action");
    click(surface, snap.rect().center());
    check(snapped && snap.isChecked(), "a toggle turns on");
    click(surface, snap.rect().center());
    check(!snapped && !snap.isChecked(), "and off");
    check(snap.rect().x > save.rect().right(), "the separator sits between");
    static_cast<void>(paint(surface));
}

void slider() {
    Surface surface;
    surface.setSize({300, 60});
    Slider &s = static_cast<Slider &>(surface.root().add(std::make_unique<Slider>(0.0, 100.0, 20.0)));
    int changes = 0;
    ScopedConnection c = s.valueChanged.connect([&changes](double) { ++changes; });
    surface.layout();
    // The track runs between the thumb's radius (7) at each end.
    const float track = s.rect().width - 14.0f;
    const auto at = [&](double fraction) { return s.rect().x + 7.0f + track * float(fraction); };

    click(surface, {at(0.5), s.rect().center().y});
    checkNear(s.value(), 50.0, 1e-4, "a click on the track jumps there");
    surface.dispatch(pointer(PointerEvent::Type::Press, at(0.5), 13));
    surface.dispatch(pointer(PointerEvent::Type::Move, at(0.75), 13));
    checkNear(s.value(), 75.0, 1e-4, "dragging follows the pointer");
    surface.dispatch(pointer(PointerEvent::Type::Move, s.rect().x + 1000, 13));
    checkNear(s.value(), 100.0, 1e-9, "clamped at the end");
    surface.dispatch(pointer(PointerEvent::Type::Release, s.rect().x + 1000, 13));
    surface.setFocus(&s);
    surface.dispatch(key(Key::Left));
    checkNear(s.value(), 99.0, 1e-9, "Left steps a hundredth");
    surface.dispatch(key(Key::Left, Modifier::Shift));
    checkNear(s.value(), 89.0, 1e-9, "Shift steps ten times as far");
    surface.dispatch(key(Key::Home));
    checkNear(s.value(), 0.0, 1e-9, "Home goes to the minimum");
    const int before = changes;
    surface.dispatch(key(Key::Home));
    checkEqual(changes, before, "no change, no signal");
    s.setValue(500);
    checkNear(s.value(), 100.0, 1e-9, "setValue clamps");
    static_cast<void>(paint(surface));
}

void progress() {
    TimePoint time{std::chrono::seconds(5)};
    Surface surface;
    surface.clock = [&time] { return time; };
    surface.setSize({200, 20});
    ProgressBar &bar = static_cast<ProgressBar &>(surface.root().add(std::make_unique<ProgressBar>()));
    bar.setValue(1.5f);
    checkNear(bar.value(), 1.0f, 1e-6f, "clamped to 1");
    static_cast<void>(paint(surface));
    check(!surface.nextTimer().has_value(), "a determinate bar needs no timer");

    bar.setBusy(true);
    static_cast<void>(paint(surface));
    check(surface.nextTimer().has_value(), "a busy bar animates");
    static_cast<void>(surface.takeDamage());
    time += std::chrono::milliseconds(20);
    surface.runTimers();
    check(!surface.takeDamage().isEmpty(), "each tick repaints it");
    bar.setBusy(false);
    check(!surface.nextTimer().has_value(), "stopping stops the timer");

    bar.setBusy(true);
    static_cast<void>(paint(surface));
    surface.root().remove(bar); // destroyed with its timer running
    check(!surface.nextTimer().has_value(), "a destroyed bar leaves no timer behind");
}

} // namespace

int main() {
    icons();
    buttons();
    submenus();
    menuBar();
    toolBar();
    slider();
    progress();
    return cfw::test::finish("ChromeTest");
}
