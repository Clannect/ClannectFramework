// A cfw-ui surface in a real native window: the first frame paints, an idle
// frame does not, input from the window reaches the controls and repaints,
// and what the window system shows is what the surface painted. Skips
// without a display.

#include <chrono>
#include <cstdio>

#include "cfw/app/UiWindow.h"
#include "cfw/test/Check.h"
#include "cfw/ui/Controls.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

PointerEvent at(PointerEvent::Type type, Vec2 p) {
    PointerEvent e;
    e.type = type;
    e.position = p;
    e.button = type == PointerEvent::Type::Move ? PointerButton::None : PointerButton::Left;
    return e;
}

} // namespace

int main() {
    auto created = UiWindow::create({"UiWindow test", {240, 120}, true, true}, Theme::dark().withSystemFonts());
    if (!created) {
        std::printf("UiWindowTest: skipped (%s)\n", created.error().message().c_str());
        return cfw::test::finish("UiWindowTest");
    }
    std::unique_ptr<UiWindow> ui = std::move(created).value();
    Stack &column = static_cast<Stack &>(ui->surface().root().add(std::make_unique<Stack>(Stack::Direction::Column, 8.0f, 10.0f)));
    Button &button = column.add<Button>("Play");
    TextField &field = column.add<TextField>();
    int clicks = 0;
    ScopedConnection c = button.clicked.connect([&] { ++clicks; });

    for (int i = 0; i < 20; ++i) {
        processEvents(std::chrono::milliseconds(10));
        ui->frame();
    }
    check(ui->framesPainted() >= 1, "the first frame paints");
    const std::size_t painted = ui->framesPainted();
    check(!ui->frame(), "an idle frame paints nothing");
    checkEqual(ui->framesPainted(), painted, "idle stays idle");

    // Input as the window system delivers it.
    const Vec2 centre = button.rect().center();
    ui->window().pointer.emit(at(PointerEvent::Type::Move, centre));
    check(ui->frame(), "hovering repaints");
    auto shot = ui->window().capture();
    check(bool(shot), "the window can be captured");
    if (shot) {
        const float ratio = ui->window().devicePixelRatio();
        const std::uint8_t *p = shot.value().row(std::uint32_t((button.rect().y + 3) * ratio)).data() +
                                std::size_t((button.rect().x + 3) * ratio) * 4;
        checkEqual(Color::fromRgba8(p[0], p[1], p[2]), ui->surface().theme().controlHover,
                   "the window shows the hovered button");
    }
    ui->window().pointer.emit(at(PointerEvent::Type::Press, centre));
    ui->window().pointer.emit(at(PointerEvent::Type::Release, centre));
    checkEqual(clicks, 1, "a click from the window reaches the button");

    ui->window().pointer.emit(at(PointerEvent::Type::Press, field.rect().center()));
    ui->window().pointer.emit(at(PointerEvent::Type::Release, field.rect().center()));
    ui->window().text.emit(TextEvent{"héllo"});
    checkEqual(field.text(), String("héllo"), "typed text reaches the focused field");
    KeyEvent selectAll{KeyEvent::Type::Press, Key::A, Modifier::Control};
    KeyEvent copy{KeyEvent::Type::Press, Key::C, Modifier::Control};
    ui->window().key.emit(selectAll);
    ui->window().key.emit(copy);
    checkEqual(clipboardText(), String("héllo"), "copy goes to the system clipboard");

    ui->window().closeRequested.emit();
    check(!ui->isOpen(), "closing the window ends the app loop");
    return cfw::test::finish("UiWindowTest");
}
