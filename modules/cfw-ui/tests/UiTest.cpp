// cfw-ui foundations, headless: stack layout, measure caching, pointer
// routing (hover, press capture, click), focus and Tab order, keyboard
// activation, disabled elements, removal, damage, and painting.

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/test/Check.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Views.h"

#include <chrono>
#include <functional>

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

// Counts how often it is measured.
class Probe : public Element {
public:
    explicit Probe(Vec2 size) : m_size(size) {}
    int measured = 0;

protected:
    Vec2 measureContent(Vec2) override {
        ++measured;
        return m_size;
    }

private:
    Vec2 m_size;
};

// Runs `onRelease` when a press on it is released (wherever that is).
class ReleaseProbe : public Element {
public:
    std::function<void()> onRelease;

protected:
    Vec2 measureContent(Vec2) override { return {100, 30}; }
    bool onPointer(const PointerEvent &event) override {
        if (event.type == PointerEvent::Type::Release && onRelease) {
            onRelease();
        }
        return event.type != PointerEvent::Type::Move;
    }
};

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

void click(Surface &surface, float x, float y) {
    surface.dispatch(pointer(PointerEvent::Type::Move, x, y));
    surface.dispatch(pointer(PointerEvent::Type::Press, x, y));
    surface.dispatch(pointer(PointerEvent::Type::Release, x, y));
}

void stacksLayOut() {
    Surface surface;
    surface.setSize({200, 300});
    auto &column = static_cast<Stack &>(surface.root().add(std::make_unique<Stack>(Stack::Direction::Column, 5.0f, 10.0f)));
    Probe &a = column.add<Probe>(Vec2{50, 20});
    Probe &b = column.add<Probe>(Vec2{80, 30});
    b.setStretch(1);
    Probe &c = column.add<Probe>(Vec2{10, 40});
    surface.layout();

    checkEqual(a.rect(), RectF{10, 10, 180, 20}, "first child: padding, full width");
    checkEqual(b.rect(), RectF{10, 35, 180, 300 - 20 - 20 - 40 - 10}, "the stretching child takes the spare height");
    checkEqual(c.rect().bottom(), 290.0f, "the last child ends at the bottom padding");

    // Rows share spare space by stretch.
    Surface rows;
    rows.setSize({300, 40});
    auto &row = static_cast<Stack &>(rows.root().add(std::make_unique<Stack>(Stack::Direction::Row, 0.0f)));
    Probe &left = row.add<Probe>(Vec2{100, 10});
    left.setStretch(1);
    Probe &right = row.add<Probe>(Vec2{100, 10});
    right.setStretch(3);
    rows.layout();
    checkEqual(left.rect().width, 125.0f, "a quarter of the spare width");
    checkEqual(right.rect().width, 175.0f, "three quarters");
    checkEqual(left.rect().height, 40.0f, "rows fill the cross axis");

    // Too little room: the stretching child gives way, the fixed ones stay
    // whole and inside the stack.
    Surface tight;
    tight.setSize({100, 100});
    auto &squeezed = static_cast<Stack &>(tight.root().add(std::make_unique<Stack>(Stack::Direction::Column, 0.0f)));
    Probe &header = squeezed.add<Probe>(Vec2{10, 30});
    Probe &body = squeezed.add<Probe>(Vec2{10, 200});
    body.setStretch(1);
    Probe &footer = squeezed.add<Probe>(Vec2{10, 20});
    tight.layout();
    checkEqual(header.rect().height, 30.0f, "a fixed child keeps its height");
    checkEqual(body.rect().height, 50.0f, "the stretching child shrinks to what is left");
    checkEqual(footer.rect().bottom(), 100.0f, "the last child stays on screen");

    // Changing one child re-measures it and its ancestors, not its siblings.
    const int before = c.measured;
    b.setFixedSize({0, 50});
    surface.layout();
    checkEqual(c.measured, before, "a sibling is not measured again");
    checkEqual(b.rect().height, 300.0f - 20 - 20 - 40 - 10, "a fixed height still stretches into spare space");
    check(!surface.needsLayout(), "layout is done");
}

void pointerRouting() {
    Surface surface;
    surface.setSize({300, 100});
    auto &row = static_cast<Stack &>(surface.root().add(std::make_unique<Stack>(Stack::Direction::Row, 10.0f, 10.0f)));
    Button &ok = row.add<Button>("OK");
    ok.setFixedSize({100, 30});
    Button &cancel = row.add<Button>("Cancel");
    cancel.setFixedSize({100, 30});
    int okClicks = 0;
    int cancelClicks = 0;
    ScopedConnection c1 = ok.clicked.connect([&] { ++okClicks; });
    ScopedConnection c2 = cancel.clicked.connect([&] { ++cancelClicks; });
    surface.layout();

    surface.dispatch(pointer(PointerEvent::Type::Move, 50, 20));
    check(ok.isHovered(), "hover follows the pointer");
    click(surface, 50, 20);
    checkEqual(okClicks, 1, "press and release inside clicks");
    check(ok.hasFocus(), "pressing a button focuses it");

    // Press, drag off, release: no click, and nothing else is hovered meanwhile.
    surface.dispatch(pointer(PointerEvent::Type::Press, 50, 20));
    check(ok.isPressed(), "the button is pressed");
    surface.dispatch(pointer(PointerEvent::Type::Move, 160, 20));
    check(!cancel.isHovered() && !ok.isHovered(), "while pressed, only the pressed element can be hovered");
    surface.dispatch(pointer(PointerEvent::Type::Release, 160, 20));
    checkEqual(okClicks, 1, "releasing elsewhere does not click");
    checkEqual(cancelClicks, 0, "nor clicks what it was released over");
    check(cancel.isHovered(), "after release, hover catches up");

    // Disabled: no click, no focus.
    cancel.setEnabled(false);
    click(surface, 160, 20);
    checkEqual(cancelClicks, 0, "a disabled button does not click");
    check(!cancel.hasFocus(), "nor takes focus");

    // Clicking the background clears focus.
    click(surface, 290, 90);
    check(surface.focus() == nullptr, "clicking empty space clears focus");

    // Removing the hovered element forgets it.
    surface.dispatch(pointer(PointerEvent::Type::Move, 50, 20));
    std::unique_ptr<Element> removed = row.remove(ok);
    check(surface.hovered() != removed.get(), "a removed element is no longer hovered");

    // A release whose handler destroys what the pointer is over (a panel
    // rebuilt on selection): hover settles on what is there now. (Found by
    // the engine's soak test; ASan saw the old element being hovered.)
    {
        Surface s2;
        s2.setSize({300, 100});
        auto &line = static_cast<Stack &>(s2.root().add(std::make_unique<Stack>(Stack::Direction::Row, 0.0f, 0.0f)));
        auto &source = static_cast<ReleaseProbe &>(line.add(std::make_unique<ReleaseProbe>()));
        Element *victim = &line.add<Button>("Rebuilt");
        victim->setFixedSize({100, 30});
        source.onRelease = [&] {
            std::unique_ptr<Element> gone = line.remove(*victim);
            victim = &line.add<Button>("Fresh");
            victim->setFixedSize({100, 30});
        };
        s2.layout();
        s2.dispatch(pointer(PointerEvent::Type::Press, 50, 15));
        s2.dispatch(pointer(PointerEvent::Type::Move, 150, 15));
        s2.dispatch(pointer(PointerEvent::Type::Release, 150, 15));
        s2.layout();
        check(s2.hovered() == nullptr || s2.hovered() == victim || s2.hovered() == &line || s2.hovered() == &s2.root(),
              "hover never lands on a destroyed element");
        s2.dispatch(pointer(PointerEvent::Type::Move, 150, 15));
        check(s2.hovered() == victim, "and then follows the pointer as usual");
    }
}

void keyboard() {
    Surface surface;
    surface.setSize({400, 100});
    auto &row = static_cast<Stack &>(surface.root().add(std::make_unique<Stack>(Stack::Direction::Row)));
    Button &a = row.add<Button>("A");
    Button &b = row.add<Button>("B");
    Button &c = row.add<Button>("C");
    row.add<Label>("not focusable");
    int bClicks = 0;
    ScopedConnection connection = b.clicked.connect([&] { ++bClicks; });

    surface.dispatch(key(Key::Tab));
    check(a.hasFocus(), "Tab focuses the first control");
    surface.dispatch(key(Key::Tab));
    check(b.hasFocus(), "and then the next");
    surface.dispatch(key(Key::Space));
    checkEqual(bClicks, 1, "Space clicks the focused button");
    surface.dispatch(key(Key::Enter));
    checkEqual(bClicks, 2, "so does Enter");
    KeyEvent repeat = key(Key::Space);
    repeat.repeat = true;
    surface.dispatch(repeat);
    checkEqual(bClicks, 2, "holding the key does not repeat the click");
    surface.dispatch(key(Key::Tab));
    surface.dispatch(key(Key::Tab));
    check(a.hasFocus(), "Tab wraps around, skipping the label");
    surface.dispatch(key(Key::Tab, Modifier::Shift));
    check(c.hasFocus(), "Shift+Tab goes back");
    c.setVisible(false);
    check(surface.focus() == nullptr, "hiding the focused control drops focus");
}

void damageAndPaint() {
    Surface surface(Theme::dark().withSystemFonts());
    surface.setSize({200, 80});
    auto &column = static_cast<Stack &>(surface.root().add(std::make_unique<Stack>(Stack::Direction::Column, 0.0f, 20.0f)));
    Button &button = column.add<Button>("Play");
    surface.layout();
    static_cast<void>(surface.takeDamage());

    surface.dispatch(pointer(PointerEvent::Type::Move, 100, 30));
    checkEqual(surface.takeDamage(), button.rect(), "hovering damages exactly the button");
    checkEqual(surface.takeDamage(), RectF{}, "damage is taken once");

    Image image = Image::create(200, 80, AlphaMode::Premultiplied).value();
    {
        RasterPaintBackend backend(image);
        Painter painter(backend);
        surface.paint(painter);
    }
    const auto at = [&image](int x, int y) {
        const std::uint8_t *p = image.row(std::uint32_t(y)).data() + std::size_t(x) * 4;
        return Color::fromRgba8(p[0], p[1], p[2]);
    };
    checkEqual(at(5, 5), surface.theme().window, "the window background");
    checkEqual(at(int(button.rect().x) + 4, int(button.rect().y) + 4), surface.theme().controlHover, "a hovered button");
    if (surface.theme().font) {
        int ink = 0;
        for (int y = int(button.rect().y); y < int(button.rect().bottom()); ++y) {
            for (int x = int(button.rect().x); x < int(button.rect().right()); ++x) {
                ink += at(x, y).r > 0.8f ? 1 : 0;
            }
        }
        check(ink > 10, "the label is drawn");
    }
    check(button.measure({200, 80}).y == surface.theme().controlHeight, "buttons are the theme's height");
}

TextEvent typed(StringView text) { return TextEvent{String(text)}; }

KeyEvent ctrlKey(Key k, Modifier extra = Modifier::None) { return key(k, Modifier::Control | extra); }

void textField() {
    Surface surface(Theme::dark().withSystemFonts());
    surface.setSize({300, 40});
    TextField &field = surface.root().add<TextField>();
    surface.layout();
    int changes = 0;
    int finished = 0;
    ScopedConnection c1 = field.textChanged.connect([&](const String &) { ++changes; });
    ScopedConnection c2 = field.editingFinished.connect([&] { ++finished; });

    surface.setFocus(&field);
    surface.dispatch(typed("héllo"));
    checkEqual(field.text(), String("héllo"), "typing inserts UTF-8 text");
    checkEqual(field.caret(), std::size_t{5}, "the caret counts code points");
    surface.dispatch(typed("\n\t"));
    checkEqual(field.text(), String("héllo"), "control characters are not inserted");

    surface.dispatch(key(Key::Left, Modifier::Shift));
    surface.dispatch(key(Key::Left, Modifier::Shift));
    checkEqual(field.selectedText(), String("lo"), "Shift+Left selects");
    surface.dispatch(ctrlKey(Key::C));
    surface.dispatch(key(Key::Backspace));
    checkEqual(field.text(), String("hél"), "Backspace deletes the selection");
    surface.dispatch(key(Key::Home));
    surface.dispatch(ctrlKey(Key::V));
    checkEqual(field.text(), String("lohél"), "paste at the caret");
    surface.dispatch(ctrlKey(Key::Z));
    checkEqual(field.text(), String("hél"), "undo");
    surface.dispatch(ctrlKey(Key::Z));
    checkEqual(field.text(), String("héllo"), "undo again");
    surface.dispatch(ctrlKey(Key::Y));
    checkEqual(field.text(), String("hél"), "redo");
    surface.dispatch(ctrlKey(Key::A));
    surface.dispatch(ctrlKey(Key::X));
    checkEqual(field.text(), String(), "cut everything");
    checkEqual(surface.clipboardText(), String("hél"), "cut goes to the clipboard");
    surface.dispatch(key(Key::Delete));
    checkEqual(field.text(), String(), "Delete on empty text does nothing");
    check(changes >= 5, "every edit reports a change");

    field.setMaxLength(3);
    surface.dispatch(typed("abcdef"));
    checkEqual(field.text(), String("abc"), "the length limit holds");
    surface.dispatch(key(Key::Enter));
    checkEqual(finished, 1, "Enter finishes editing");
    surface.setFocus(nullptr);
    checkEqual(finished, 1, "losing focus without an edit does not finish again");

    // Pointer: clicking at the far right puts the caret at the end; a
    // double-click selects all.
    if (surface.theme().font) {
        surface.dispatch(pointer(PointerEvent::Type::Press, 290, 20));
        surface.dispatch(pointer(PointerEvent::Type::Release, 290, 20));
        check(field.hasFocus(), "clicking focuses the field");
        checkEqual(field.caret(), std::size_t{3}, "the caret goes where the pointer is");
        surface.dispatch(pointer(PointerEvent::Type::Press, 3, 20));
        checkEqual(field.caret(), std::size_t{0}, "the start of the text");
        surface.dispatch(pointer(PointerEvent::Type::Move, 290, 20));
        surface.dispatch(pointer(PointerEvent::Type::Release, 290, 20));
        checkEqual(field.selectedText(), String("abc"), "dragging selects");
    }
}

void scrollArea() {
    Surface surface;
    surface.setSize({200, 100});
    ScrollArea &area = surface.root().add<ScrollArea>();
    Stack &list = area.setContent<Stack>(Stack::Direction::Column, 0.0f);
    std::vector<Probe *> rows;
    for (int i = 0; i < 10; ++i) {
        rows.push_back(&list.add<Probe>(Vec2{50, 30}));
    }
    surface.layout();
    checkEqual(area.maxOffset().y, 200.0f, "300 of content in 100 of view");
    checkEqual(rows[0]->rect().width, 200.0f - ScrollArea::kBarWidth, "content leaves room for the bar");

    PointerEvent wheel = pointer(PointerEvent::Type::Wheel, 50, 50);
    wheel.wheelDelta = {0, -40};
    surface.dispatch(wheel);
    checkEqual(area.offset().y, 40.0f, "the wheel scrolls");
    checkEqual(rows[0]->rect().y, -40.0f, "and moves the content");
    wheel.wheelDelta = {0, -1000};
    surface.dispatch(wheel);
    checkEqual(area.offset().y, 200.0f, "never past the end");

    area.ensureVisible(rows[1]->rect());
    checkEqual(rows[1]->rect().y, 0.0f, "ensureVisible brings a row into view");

    // Dragging the thumb (at the top) to the bottom scrolls to the end.
    area.scrollTo({});
    surface.dispatch(pointer(PointerEvent::Type::Press, 195, 5));
    surface.dispatch(pointer(PointerEvent::Type::Move, 195, 500));
    surface.dispatch(pointer(PointerEvent::Type::Release, 195, 500));
    checkEqual(area.offset().y, 200.0f, "dragging the thumb scrolls");

    // Content that fits needs no bar and no scrolling.
    for (int i = 0; i < 8; ++i) {
        static_cast<void>(list.remove(*rows[std::size_t(i)]));
    }
    surface.layout();
    checkEqual(area.maxOffset().y, 0.0f, "short content does not scroll");
    checkEqual(rows[9]->rect().width, 200.0f, "and gets the full width");
}

void choices() {
    Surface surface(Theme::dark().withSystemFonts());
    surface.setSize({400, 300});
    Stack &column = static_cast<Stack &>(surface.root().add(std::make_unique<Stack>(Stack::Direction::Column, 4.0f, 10.0f)));

    // CheckBox
    CheckBox &box = column.add<CheckBox>("Anchored");
    std::vector<bool> toggles;
    ScopedConnection c1 = box.toggled.connect([&](bool on) { toggles.push_back(on); });
    surface.layout();
    click(surface, box.rect().x + 5, box.rect().y + 5);
    surface.dispatch(key(Key::Space));
    check(toggles == std::vector<bool>{true, false}, "click and Space toggle the check box");

    // NumberField
    NumberField &number = column.add<NumberField>(1.0, 2);
    number.setRange(-10.0, 10.0);
    number.setStep(0.5);
    std::vector<double> values;
    ScopedConnection c2 = number.valueChanged.connect([&](double v) { values.push_back(v); });
    surface.layout();
    checkEqual(number.text(), String("1.00"), "the value is shown with its decimals");
    surface.setFocus(&number);
    number.selectAll();
    surface.dispatch(typed("3.14159"));
    surface.dispatch(key(Key::Enter));
    checkNear(number.value(), 3.14, 1e-9, "typing and Enter commits, rounded");
    number.selectAll();
    surface.dispatch(typed("99"));
    surface.dispatch(key(Key::Enter));
    checkNear(number.value(), 10.0, 1e-9, "clamped to the range");
    surface.dispatch(key(Key::Down));
    checkNear(number.value(), 9.5, 1e-9, "Down steps");
    number.selectAll();
    surface.dispatch(typed("abc"));
    surface.dispatch(key(Key::Enter));
    checkNear(number.value(), 9.5, 1e-9, "text that is not a number is refused");
    checkEqual(number.text(), String("9.50"), "and the last value comes back");
    checkEqual(values.size(), std::size_t{3}, "each change reported once");

    // Dropdown
    Dropdown &drop = column.add<Dropdown>(std::vector<String>{"Stretch", "Fit", "Crop", "Tile"}, 0);
    std::vector<int> chosen;
    ScopedConnection c3 = drop.currentChanged.connect([&](int i) { chosen.push_back(i); });
    surface.layout();
    const RectF dropRect = drop.rect();
    click(surface, dropRect.x + 10, dropRect.y + 10);
    check(drop.isOpen() && surface.popupCount() == 1, "clicking opens the list");
    surface.dispatch(key(Key::Down));
    surface.dispatch(key(Key::Down));
    surface.dispatch(key(Key::Enter));
    check(chosen == std::vector<int>{2}, "arrow keys then Enter choose");
    check(!drop.isOpen() && surface.popupCount() == 0, "choosing closes the list");
    checkEqual(drop.currentText(), String("Crop"), "the choice is shown");

    click(surface, dropRect.x + 10, dropRect.y + 10);
    surface.layout();
    click(surface, 390, 290); // outside
    check(!drop.isOpen(), "a click outside closes the list");
    checkEqual(chosen.size(), std::size_t{1}, "without choosing");
    click(surface, dropRect.x + 10, dropRect.y + 10);
    surface.dispatch(key(Key::Escape));
    check(!drop.isOpen(), "Escape closes the list");

    // Pointer choice: the list sits below the dropdown; its second item.
    click(surface, dropRect.x + 10, dropRect.y + 10);
    surface.layout();
    const float itemY = dropRect.bottom() + 2.0f + 4.0f + surface.theme().controlHeight * 1.5f;
    click(surface, dropRect.x + 20, itemY);
    checkEqual(drop.currentText(), String("Fit"), "clicking an item chooses it");

    // A context menu runs its action and closes.
    int copies = 0;
    auto menu = std::make_unique<Menu>();
    menu->addItem("Copy", [&] { ++copies; }, "Ctrl+C");
    menu->addSeparator();
    menu->addItem("Delete", nullptr);
    Menu &opened = Menu::popup(surface, std::move(menu), {200, 100});
    surface.layout();
    const RectF first = opened.children()[0]->rect();
    click(surface, first.x + 10, first.y + 5);
    checkEqual(copies, 1, "a menu item runs its action");
    checkEqual(surface.popupCount(), std::size_t{0}, "and closes the menu");

    // Destroying a dropdown with its list open is safe.
    click(surface, dropRect.x + 10, dropRect.y + 10);
    std::unique_ptr<Element> gone = column.remove(drop);
    gone.reset();
    checkEqual(surface.popupCount(), std::size_t{0}, "its list goes with it");
    surface.dispatch(key(Key::Down));

    // Nothing chosen shows the placeholder; a searchable list filters.
    Dropdown &materials = column.add<Dropdown>(std::vector<String>{"Plastic", "Wood", "Brick", "Wood Planks"}, 0);
    materials.setPlaceholder("Mixed");
    materials.setCurrentIndex(-1);
    checkEqual(materials.currentIndex(), -1, "-1 chooses nothing");
    checkEqual(materials.currentText(), String(), "so there is no current text");
    materials.setSearchable(true, "Search materials...");
    std::vector<int> picked;
    ScopedConnection c4 = materials.currentChanged.connect([&](int i) { picked.push_back(i); });
    surface.layout();
    materials.open();
    check(surface.focus() && dynamic_cast<TextField *>(surface.focus()), "the search field has the focus");
    surface.dispatch(typed("wood"));
    int shown = 0;
    for (const auto &child : surface.focus()->parent()->children()) {
        shown += dynamic_cast<MenuItem *>(child.get()) && child->isVisible() ? 1 : 0;
    }
    checkEqual(shown, 2, "typing hides what does not match");
    surface.dispatch(key(Key::Enter));
    check(picked == std::vector<int>{1}, "Enter chooses the first match");
    check(!materials.isOpen(), "and closes the list");
    column.remove(materials).reset();

    // Mixed check boxes and refused text.
    CheckBox &mixed = column.add<CheckBox>("Anchored");
    mixed.setPartial(true);
    std::vector<bool> mixedToggles;
    ScopedConnection c5 = mixed.toggled.connect([&](bool on) { mixedToggles.push_back(on); });
    surface.setFocus(&mixed);
    surface.dispatch(key(Key::Space));
    check(mixedToggles == std::vector<bool>{true} && !mixed.isPartial(), "a mixed box becomes checked");
    column.remove(mixed).reset();
    TextField &refused = column.add<TextField>("abc");
    refused.setInvalid(true);
    check(refused.isInvalid(), "a field can be marked invalid");
    String copied = "nothing";
    surface.writeClipboard = [&](StringView text) { copied = String(text); };
    refused.setMasked(true);
    surface.setFocus(&refused);
    refused.selectAll();
    KeyEvent copy = key(Key::C);
    copy.modifiers = Modifier::Control;
    surface.dispatch(copy);
    check(copied == "nothing", "a masked field is not copied");
    checkEqual(refused.text(), String("abc"), "but keeps its text");
    // Plain keys that type belong to the field, not to window shortcuts.
    int zooms = 0;
    const auto zoom = surface.addShortcut(KeyChord::parse("F").value(), [&] { ++zooms; });
    refused.setMasked(false);
    surface.setFocus(&refused);
    surface.dispatch(key(Key::F));
    checkEqual(zooms, 0, "typing F in a field is not the F shortcut");
    surface.setFocus(nullptr);
    surface.dispatch(key(Key::F));
    checkEqual(zooms, 1, "elsewhere it is");
    surface.removeShortcut(zoom);
    column.remove(refused).reset();

    // Painting everything with a popup open does not fail.
    Dropdown &again = column.add<Dropdown>(std::vector<String>{"A", "B"}, 0);
    surface.layout();
    again.open();
    Image image = Image::create(400, 300, AlphaMode::Premultiplied).value();
    RasterPaintBackend backend(image);
    Painter painter(backend);
    surface.paint(painter);
    check(surface.popupCount() == 1, "painted with a popup open");
}

// 1000 folders of 100 items: ids 1..1000 are folders, folder f's items are
// f * 1000 + 1 .. f * 1000 + 100.
class BigModel : public TreeModel {
public:
    std::size_t folders = 1000;
    std::size_t childCount(Id parent) const override { return parent == kRoot ? folders : parent <= 1000 ? 100 : 0; }
    Id child(Id parent, std::size_t i) const override { return parent == kRoot ? Id(i + 1) : parent * 1000 + i + 1; }
    String text(Id node) const override { return (node <= 1000 ? "Folder " : "Part ") + std::to_string(node); }
};

void views() {
    Surface surface(Theme::dark().withSystemFonts());
    surface.setSize({400, 300});
    Splitter &split = surface.root().add<Splitter>(Stack::Direction::Row, 0.5f);
    BigModel model;
    TreeView &tree = static_cast<TreeView &>(split.setFirst(std::make_unique<TreeView>(model)));
    Stack &right = static_cast<Stack &>(split.setSecond(std::make_unique<Stack>(Stack::Direction::Column)));
    TabBar &tabs = right.add<TabBar>(std::vector<String>{"Properties", "Attributes"});
    surface.layout();

    // Splitter
    checkNear(tree.rect().width, (400.0f - Splitter::kHandle) / 2, 1.0, "the splitter shares the width");
    const float handleX = tree.rect().right() + 2;
    surface.dispatch(pointer(PointerEvent::Type::Press, handleX, 100));
    surface.dispatch(pointer(PointerEvent::Type::Move, 300, 100));
    surface.dispatch(pointer(PointerEvent::Type::Release, 300, 100));
    checkNear(tree.rect().right(), 298.0f, 2.0, "dragging the handle resizes the panes");
    surface.dispatch(pointer(PointerEvent::Type::Press, 298, 100));
    surface.dispatch(pointer(PointerEvent::Type::Move, 395, 100));
    surface.dispatch(pointer(PointerEvent::Type::Release, 395, 100));
    check(right.rect().width >= 40.0f, "a pane keeps its minimum size");
    split.setRatio(0.5f);

    // TabBar
    int tab = -1;
    ScopedConnection c1 = tabs.currentChanged.connect([&](int i) { tab = i; });
    click(surface, tabs.rect().x + tabs.measure({400, 300}).x - 5, tabs.rect().y + 5);
    checkEqual(tab, 1, "clicking a tab selects it");
    surface.dispatch(key(Key::Left));
    checkEqual(tab, 0, "Left goes to the previous tab");
    const float plainWidth = tabs.measure({400, 300}).x;
    tabs.setTabBadge(1, "12", Color{0.4f, 0.1f, 0.1f, 1.0f}, Color{1, 1, 1, 1});
    const float badged = tabs.measure({400, 300}).x;
    check(badged > plainWidth + 14.0f, "a badge widens its tab");
    tabs.setTabIcon(0, Icon::fromSvg(R"(<svg viewBox="0 0 24 24"><rect width="24" height="24"/></svg>)"));
    check(tabs.measure({400, 300}).x > badged + 14.0f, "and so does an icon");
    tabs.setTabBadge(1, "", Color{}, Color{});
    tabs.setTabIcon(0, Icon());
    checkNear(tabs.measure({400, 300}).x, plainWidth, 0.01, "removing both restores the width");

    // TreeView: a scene of 101,000 instances.
    checkEqual(tree.rowCount(), std::size_t{1000}, "collapsed folders show one row each");
    const auto start = std::chrono::steady_clock::now();
    std::vector<TreeModel::Id> allFolders;
    for (TreeModel::Id f = 1; f <= 1000; ++f) {
        allFolders.push_back(f);
    }
    tree.setExpanded(allFolders, true);
    checkEqual(tree.rowCount(), std::size_t{101000}, "expanding everything");
    Image image = Image::create(400, 300, AlphaMode::Premultiplied).value();
    RasterPaintBackend backend(image);
    Painter painter(backend);
    tree.scrollTo(1e9f);
    surface.paint(painter);
    const std::size_t visibleRows = std::size_t(tree.rect().height / tree.rowHeight()) + 2;
    check(tree.rowsPainted() <= visibleRows, "only the visible rows are painted");
    check(tree.rowsPainted() > 0, "at the end of 101,000 rows");
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("  101,000-row tree: expand and paint in %.0f ms\n", seconds * 1000);

    // Selection and the keyboard.
    tree.scrollTo(0);
    tree.setExpanded(allFolders, false);
    int changes = 0;
    ScopedConnection c2 = tree.selectionChanged.connect([&] { ++changes; });
    std::vector<TreeModel::Id> activatedIds;
    ScopedConnection c3 = tree.activated.connect([&](TreeModel::Id id) { activatedIds.push_back(id); });
    const float rowH = tree.rowHeight();
    const auto rowY = [&](int row) { return tree.rect().y + rowH * float(row) + rowH / 2; };
    click(surface, tree.rect().x + 60, rowY(1));
    check(tree.selection() == std::vector<TreeModel::Id>{2}, "clicking a row selects it");
    check(tree.hasFocus(), "and focuses the tree");
    PointerEvent shiftClick = pointer(PointerEvent::Type::Press, tree.rect().x + 60, rowY(3));
    shiftClick.modifiers = Modifier::Shift;
    surface.dispatch(shiftClick);
    surface.dispatch(pointer(PointerEvent::Type::Release, tree.rect().x + 60, rowY(3)));
    check(tree.selection() == std::vector<TreeModel::Id>{2, 3, 4}, "Shift+click selects a range");
    PointerEvent ctrlClick = pointer(PointerEvent::Type::Press, tree.rect().x + 60, rowY(3));
    ctrlClick.modifiers = Modifier::Control;
    surface.dispatch(ctrlClick);
    surface.dispatch(pointer(PointerEvent::Type::Release, tree.rect().x + 60, rowY(3)));
    check(tree.selection() == std::vector<TreeModel::Id>{2, 3}, "Ctrl+click toggles one row");
    surface.dispatch(key(Key::Right));
    check(tree.isExpanded(4), "Right expands");
    surface.dispatch(key(Key::Right));
    checkEqual(tree.current(), TreeModel::Id{4001}, "and then goes to the first child");
    surface.dispatch(key(Key::Left));
    checkEqual(tree.current(), TreeModel::Id{4}, "Left goes to the parent");
    surface.dispatch(key(Key::Left));
    check(!tree.isExpanded(4), "and then collapses");
    surface.dispatch(key(Key::Down, Modifier::Shift));
    check(tree.selection() == std::vector<TreeModel::Id>{4, 5}, "Shift+Down extends");
    surface.dispatch(key(Key::Enter));
    check(activatedIds == std::vector<TreeModel::Id>{5}, "Enter activates");
    PointerEvent twice = pointer(PointerEvent::Type::Press, tree.rect().x + 60, rowY(0));
    twice.clickCount = 2;
    surface.dispatch(twice);
    surface.dispatch(pointer(PointerEvent::Type::Release, tree.rect().x + 60, rowY(0)));
    check(activatedIds.back() == 1, "a double-click activates");
    click(surface, tree.rect().x + 4 + 6, rowY(0)); // the disclosure triangle
    check(tree.isExpanded(1), "clicking the triangle expands");
    check(changes > 0, "selection changes are reported");

    tree.reveal({7, 7050});
    checkEqual(tree.current(), TreeModel::Id{1}, "reveal does not change the selection");
    check(tree.isExpanded(7), "reveal expands the ancestors");

    model.folders = 3;
    model.changed.emit();
    checkEqual(tree.rowCount(), std::size_t{3 + 100}, "the view follows model changes");
}

} // namespace

int main() {
    views();
    choices();
    textField();
    scrollArea();
    stacksLayOut();
    pointerRouting();
    keyboard();
    damageAndPaint();
    return cfw::test::finish("UiTest");
}
