// Input method compositions in a TextField: shown at the caret (replacing
// the selection) and underlined, the caret where the input method puts it,
// committed by the text that follows, dropped with the focus; refused by
// read-only and masked fields. And the surface's text input area, which
// places the candidate window and turns the input method off outside text.

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

CompositionEvent composing(const char *text, std::size_t cursor) {
    CompositionEvent e;
    e.text = text;
    e.cursor = cursor;
    return e;
}

// The longest horizontal run of light pixels inside `r` (text is light on
// the dark theme): an underline makes a long one, "x" glyphs only short ones.
int longestRun(const Image &image, const RectF &r) {
    int best = 0;
    for (int y = int(r.y) + 2; y < int(r.bottom()) - 2; ++y) {
        int run = 0;
        for (int x = int(r.x) + 3; x < int(r.right()) - 3; ++x) {
            const std::uint8_t *p = image.row(std::uint32_t(y)).data() + std::size_t(x) * 4;
            run = p[0] > 150 && p[1] > 150 && p[2] > 150 ? run + 1 : 0;
            best = std::max(best, run);
        }
    }
    return best;
}

void underline() {
    Surface surface(Theme::dark().withSystemFonts());
    if (!surface.theme().font) {
        return;
    }
    surface.setSize({200, 40});
    TextField &field = static_cast<TextField &>(surface.root().add(std::make_unique<TextField>()));
    surface.layout();
    surface.setFocus(&field);
    const auto render = [&] {
        Image image = Image::create(200, 40, AlphaMode::Premultiplied).value();
        RasterPaintBackend backend(image);
        Painter painter(backend);
        surface.paint(painter);
        return image;
    };
    surface.dispatch(composing("xxxx", 4));
    check(longestRun(render(), field.rect()) >= 12, "the composition is underlined");
    TextEvent commit;
    commit.text = "xxxx";
    surface.dispatch(commit);
    check(longestRun(render(), field.rect()) < 12, "committed text is not");
}

} // namespace

int main() {
    Surface surface(Theme::dark().withSystemFonts());
    surface.setSize({300, 100});
    auto &column = static_cast<Stack &>(surface.root().add(std::make_unique<Stack>(Stack::Direction::Column, 8.0f, 8.0f)));
    TextField &field = column.add<TextField>("abc");
    Button &button = column.add<Button>("OK");
    TextField &password = column.add<TextField>();
    password.setMasked(true);
    surface.layout();

    check(!surface.textInputArea().has_value(), "no focus, no text input");
    surface.setFocus(&button);
    check(!surface.textInputArea().has_value(), "a button takes no text");
    surface.setFocus(&field);
    const std::optional<RectF> area = surface.textInputArea();
    check(area.has_value() && field.rect().contains(area->center()), "a focused field reports its caret");

    // Composing over a selection: "abc" with "b" selected.
    field.setSelection(1, 2);
    check(surface.dispatch(composing("にほ", 3)), "the field takes the composition");
    checkEqual(field.composition(), String("にほ"), "it is shown");
    checkEqual(field.text(), String("abc"), "the text is untouched until committed");
    const std::optional<RectF> moved = surface.textInputArea();
    check(moved && area && moved->x > area->x - 100.0f, "the caret follows the composition");

    // The input method commits: the text replaces the selection.
    TextEvent commit;
    commit.text = "日本";
    surface.dispatch(commit);
    check(field.composition().empty(), "committing ends the composition");
    checkEqual(field.text(), String("a日本c"), "the committed text replaces the selection");
    checkEqual(field.caret(), std::size_t(3), "the caret is after it");

    // Cancelled by the input method, or dropped with the focus.
    surface.dispatch(composing("x", 1));
    surface.dispatch(composing("", 0));
    check(field.composition().empty() && field.text() == "a日本c", "an empty composition cancels");
    surface.dispatch(composing("y", 1));
    surface.setFocus(&button);
    check(field.composition().empty(), "losing focus drops the composition");
    checkEqual(field.text(), String("a日本c"), "without typing it");

    // A password field never shows composed text; a read-only one takes none.
    surface.setFocus(&password);
    check(!surface.dispatch(composing("secret", 6)), "a masked field refuses compositions");
    field.setReadOnly(true);
    surface.setFocus(&field);
    check(!surface.dispatch(composing("z", 1)), "a read-only field refuses them");
    check(!surface.textInputArea().has_value(), "and has no text input area");

    underline();
    return cfw::test::finish("CompositionTest");
}
