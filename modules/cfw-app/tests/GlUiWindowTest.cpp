// A surface with a GL view in a real window: the view renders into its own
// framebuffer and shows where it is laid out, the interface lies over it,
// popups lie over the view, and idle frames render nothing. Skips without a
// display or OpenGL.

#include <cstdio>

#include "cfw/app/GlUiWindow.h"
#include "cfw/test/Check.h"
#include "cfw/ui/Chrome.h"
#include "cfw/ui/Controls.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

std::array<std::uint8_t, 4> pixel(const Image &image, int x, int y) {
    const std::uint8_t *p = image.pixels().data() + std::size_t(y) * image.stride() + std::size_t(x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

bool near(std::array<std::uint8_t, 4> a, std::array<std::uint8_t, 4> b) {
    for (int i = 0; i < 3; ++i) {
        if (std::abs(int(a[std::size_t(i)]) - int(b[std::size_t(i)])) > 3) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    auto created = GlUiWindow::create({"GlUiWindow test", {320, 240}, true, true}, Theme::dark());
    if (!created) {
        std::printf("GlUiWindowTest: skipped (%s)\n", created.error().message().c_str());
        return cfw::test::finish("GlUiWindowTest");
    }
    std::unique_ptr<GlUiWindow> ui = std::move(created).value();
    auto &column = static_cast<Stack &>(ui->surface().root().add(std::make_unique<Stack>(Stack::Direction::Column, 0.0f)));
    auto &bar = column.add<Panel>();
    bar.setFixedSize({0, 40});
    GlView &view = column.add<GlView>();
    view.setStretch(1);
    int renders = 0;
    Vec2i size;
    view.render = [&](const GlViewTarget &target) {
        ++renders;
        size = {target.width, target.height};
        // Top half red, bottom half blue: shows the view is not flipped.
        target.gl->Enable(GL_SCISSOR_TEST);
        target.gl->Scissor(0, target.height / 2, target.width, target.height - target.height / 2);
        target.gl->ClearColor(1, 0, 0, 1);
        target.gl->Clear(GL_COLOR_BUFFER_BIT);
        target.gl->Scissor(0, 0, target.width, target.height / 2);
        target.gl->ClearColor(0, 0, 1, 1);
        target.gl->Clear(GL_COLOR_BUFFER_BIT);
        target.gl->Disable(GL_SCISSOR_TEST);
    };

    for (int i = 0; i < 20; ++i) {
        processEvents(std::chrono::milliseconds(5));
        ui->frame();
    }
    const float ratio = ui->window().devicePixelRatio();
    check(renders >= 1, "the view rendered");
    checkEqual(size.x, int(std::lround(320 * ratio)), "at its width in pixels");
    checkEqual(size.y, int(std::lround(200 * ratio)), "and its height");

    auto shot = ui->captureFrame();
    check(bool(shot), "the frame reads back");
    if (!shot) {
        return cfw::test::finish("GlUiWindowTest");
    }
    const Image &image = shot.value();
    const auto s = [ratio](float v) { return int(v * ratio); };
    const Color panel = Theme::dark().panel;
    const auto panelBytes = panel.toRgba8();
    check(near(pixel(image, s(160), s(20)), panelBytes), "the panel above the view");
    check(near(pixel(image, s(160), s(60)), {255, 0, 0, 255}), "the view's top half is red, where it is laid out");
    check(near(pixel(image, s(160), s(230)), {0, 0, 255, 255}), "and its bottom half blue");

    // Idle: nothing renders.
    const int before = renders;
    const std::size_t presented = ui->framesPresented();
    for (int i = 0; i < 5; ++i) {
        ui->frame();
    }
    checkEqual(renders, before, "an idle frame renders no view");
    checkEqual(ui->framesPresented(), presented, "and presents nothing");
    check(!ui->wantsFrame(), "and wants no frame");

    view.update();
    check(ui->wantsFrame(), "update() asks for a frame");
    ui->frame();
    checkEqual(renders, before + 1, "and renders once");
    view.setAnimating(true);
    ui->frame();
    ui->frame();
    checkEqual(renders, before + 3, "animating renders every frame");
    view.setAnimating(false);

    // A popup lies over the view.
    auto menu = std::make_unique<Menu>();
    menu->addItem("Over the view", [] {});
    menu->setFixedSize({150, 60});
    ui->surface().openPopup(std::move(menu), {50, 100});
    ui->frame();
    auto over = ui->captureFrame();
    check(bool(over) && near(pixel(over.value(), s(120), s(130)), panelBytes), "a popup covers the view");
    check(bool(over) && near(pixel(over.value(), s(300), s(130)), {255, 0, 0, 255}), "beside it the view shows");
    return cfw::test::finish("GlUiWindowTest");
}
