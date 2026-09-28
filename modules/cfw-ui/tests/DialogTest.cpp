// Dialogs, message boxes and the colour picker, headless.

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/test/Check.h"
#include "cfw/ui/Dialog.h"
#include "cfw/ui/Surface.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

PointerEvent pointer(PointerEvent::Type type, Vec2 at) {
    PointerEvent e;
    e.type = type;
    e.position = at;
    e.button = type == PointerEvent::Type::Move ? PointerButton::None : PointerButton::Left;
    return e;
}

KeyEvent key(Key k) {
    KeyEvent e;
    e.key = k;
    return e;
}

void click(Surface &surface, Vec2 at) {
    surface.dispatch(pointer(PointerEvent::Type::Move, at));
    surface.dispatch(pointer(PointerEvent::Type::Press, at));
    surface.dispatch(pointer(PointerEvent::Type::Release, at));
}

void paint(Surface &surface) {
    Image image = Image::create(std::uint32_t(surface.size().x), std::uint32_t(surface.size().y),
                                AlphaMode::Premultiplied)
                      .value();
    RasterPaintBackend backend(image);
    Painter painter(backend);
    surface.paint(painter);
}

// The buttons of the open dialog, in order.
std::vector<Button *> buttons(Element &dialog) {
    std::vector<Button *> out;
    std::vector<Element *> stack{&dialog};
    while (!stack.empty()) {
        Element *e = stack.back();
        stack.pop_back();
        if (auto *b = dynamic_cast<Button *>(e)) {
            out.push_back(b);
        }
        for (auto it = e->children().end(); it != e->children().begin();) {
            --it;
            stack.push_back(it->get());
        }
    }
    return out;
}

void messageBoxes() {
    Surface surface;
    surface.setSize({800, 600});
    surface.layout();
    std::vector<int> results;
    Dialog &box = MessageBox::show(surface, "Unsaved changes", "Save the changes to Obby before closing?",
                                   {"Save", "Don't Save", "Cancel"}, [&results](int r) { results.push_back(r); });
    surface.layout();
    check(box.rect().center().x == 400.0f, "centred across");
    checkEqual(surface.popupCount(), std::size_t(1), "open");
    const std::vector<Button *> b = buttons(box);
    checkEqual(b.size(), std::size_t(3), "three buttons");
    check(surface.focus() == b[0], "the default button has the focus");
    paint(surface);

    click(surface, b[1]->rect().center());
    check(results == std::vector<int>{1}, "a button finishes with its index");
    checkEqual(surface.popupCount(), std::size_t(0), "and closes the dialog");

    MessageBox::show(surface, "Quit", "Quit?", {"Quit", "Stay"}, [&results](int r) { results.push_back(r); }, 1);
    surface.layout();
    surface.dispatch(key(Key::Enter));
    checkEqual(results.back(), 1, "Enter presses the default button");

    MessageBox::show(surface, "Quit", "Quit?", {"Quit", "Stay"}, [&results](int r) { results.push_back(r); });
    surface.layout();
    surface.dispatch(key(Key::Escape));
    checkEqual(results.back(), Dialog::kCancelled, "Escape cancels");
    checkEqual(results.size(), std::size_t(3), "each dialog reported exactly once");

    Dialog &programmatic = MessageBox::show(surface, "Info", "Done.", {}, [&results](int r) { results.push_back(r); });
    programmatic.finish(7);
    checkEqual(results.back(), 7, "finish() reports its result");
    checkEqual(results.size(), std::size_t(4), "once");
}

void picker() {
    Surface surface;
    surface.setSize({600, 500});
    ColorPicker &p = static_cast<ColorPicker &>(surface.root().add(std::make_unique<ColorPicker>(Color{1, 0, 0, 1})));
    std::vector<Color> changes;
    ScopedConnection c = p.colorChanged.connect([&changes](Color color) { changes.push_back(color); });
    surface.layout();
    paint(surface);

    const RectF square{p.rect().x, p.rect().y, ColorPicker::kSquare, ColorPicker::kSquare * 0.8f};
    // Bottom-left of the square is black whatever the hue.
    click(surface, {square.x + 1, square.bottom() - 1});
    check(!changes.empty() && changes.back().toRgba8()[0] < 3, "the bottom of the square is black");
    // Top-right is the pure hue.
    click(surface, {square.right() - 1, square.y});
    const auto corner = changes.back().toRgba8();
    check(corner[0] == 255 && corner[1] < 3 && corner[2] < 3, "the top right is the hue");

    // The strip: a third of the way down is green-ish (hue 120).
    const RectF strip{square.right() + 10.0f, square.y, ColorPicker::kStrip, square.height};
    click(surface, {strip.center().x, strip.y + strip.height / 3.0f});
    checkNear(p.hsv().h, 120.0f, 1.0f, "the hue follows the strip");
    checkEqual(changes.back().toRgba8()[1], std::uint8_t(255), "and the colour turns green");

    // Typing a hex value.
    TextField *hex = nullptr;
    for (const auto &child : p.children()) {
        hex = dynamic_cast<TextField *>(child.get());
    }
    check(hex != nullptr, "a hex field");
    surface.setFocus(hex);
    hex->selectAll();
    surface.dispatch(TextEvent{"#3366cc"});
    surface.dispatch(key(Key::Enter));
    checkEqual(p.color().toRgba8(), (std::array<std::uint8_t, 4>{0x33, 0x66, 0xcc, 255}), "the hex value applies");
    hex->selectAll();
    surface.dispatch(TextEvent{"nonsense"});
    surface.dispatch(key(Key::Enter));
    checkEqual(p.color().toRgba8(), (std::array<std::uint8_t, 4>{0x33, 0x66, 0xcc, 255}), "garbage is ignored");
    checkEqual(hex->text(), String("#3366cc"), "and put back");
    paint(surface);
}

void swatch() {
    Surface surface;
    surface.setSize({600, 500});
    ColorSwatch &well = static_cast<ColorSwatch &>(surface.root().add(std::make_unique<ColorSwatch>(Color{0, 0, 1, 1})));
    well.setFixedSize({100, 26});
    std::vector<Color> changes;
    ScopedConnection c = well.colorChanged.connect([&changes](Color color) { changes.push_back(color); });
    surface.layout();
    paint(surface);

    click(surface, well.rect().center());
    checkEqual(surface.popupCount(), std::size_t(1), "a click opens the picker");
    surface.layout();
    paint(surface);
    auto *dialog = dynamic_cast<Dialog *>(surface.hitTest({300, 250}) ? surface.focus()->parent()->parent() : nullptr);
    check(dialog != nullptr, "in a dialog");
    // Pick the top right of the square (pure blue: the same colour) then black.
    ColorPicker *picker = nullptr;
    std::vector<Element *> stack{dialog};
    while (!stack.empty() && !picker) {
        Element *e = stack.back();
        stack.pop_back();
        picker = dynamic_cast<ColorPicker *>(e);
        for (const auto &child : e->children()) {
            stack.push_back(child.get());
        }
    }
    check(picker != nullptr, "with a picker");
    click(surface, {picker->rect().x + 1, picker->rect().y + ColorPicker::kSquare * 0.8f - 1});
    checkEqual(changes.size(), std::size_t(0), "nothing changes before OK");
    surface.dispatch(key(Key::Escape));
    checkEqual(changes.size(), std::size_t(0), "Cancel keeps the colour");
    check(well.color().toRgba8() == (std::array<std::uint8_t, 4>{0, 0, 255, 255}), "still blue");

    click(surface, well.rect().center());
    surface.layout();
    const std::vector<Button *> b = buttons(*surface.focus()->parent()->parent());
    ColorPicker *second = nullptr;
    stack = {surface.focus()->parent()->parent()};
    while (!stack.empty() && !second) {
        Element *e = stack.back();
        stack.pop_back();
        second = dynamic_cast<ColorPicker *>(e);
        for (const auto &child : e->children()) {
            stack.push_back(child.get());
        }
    }
    click(surface, {second->rect().x + 1, second->rect().y + ColorPicker::kSquare * 0.8f - 1});
    surface.dispatch(key(Key::Enter));
    checkEqual(changes.size(), std::size_t(1), "OK applies the colour");
    check(well.color().toRgba8()[2] < 3, "the well turns black");
    checkEqual(surface.popupCount(), std::size_t(0), "and the dialog closes");
}

} // namespace

int main() {
    messageBoxes();
    picker();
    swatch();
    return cfw::test::finish("DialogTest");
}
