// cfw-ui foundations, headless: stack layout, measure caching, pointer
// routing (hover, press capture, click), focus and Tab order, keyboard
// activation, disabled elements, removal, damage, and painting.

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/test/Check.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Surface.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

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

} // namespace

int main() {
    textField();
    scrollArea();
    stacksLayOut();
    pointerRouting();
    keyboard();
    damageAndPaint();
    return cfw::test::finish("UiTest");
}
