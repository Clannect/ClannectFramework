// The property grid, headless: sections and rows, collapsing, filtering,
// the shared name column, scrubbing and clicking a name.

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/test/Check.h"
#include "cfw/ui/PropertyGrid.h"
#include "cfw/ui/Surface.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

PointerEvent pointer(PointerEvent::Type type, Vec2 at, Modifier modifiers = Modifier::None) {
    PointerEvent e;
    e.type = type;
    e.position = at;
    e.button = type == PointerEvent::Type::Move ? PointerButton::None : PointerButton::Left;
    e.modifiers = modifiers;
    return e;
}

struct Panel {
    Surface surface;
    PropertyGrid *grid = nullptr;
    PropertySection *data = nullptr;
    PropertySection *appearance = nullptr;
    NumberField *transparency = nullptr;
    TextField *name = nullptr;

    Panel() {
        surface.setSize({320, 600});
        grid = &static_cast<PropertyGrid &>(surface.root().add(std::make_unique<PropertyGrid>()));
        data = &grid->addSection("Data");
        name = &data->addRow<TextField>("Name", "Baseplate");
        data->addRow<CheckBox>("Anchored", "", true);
        data->addRow<CheckBox>("CanCollide", "", true);
        appearance = &grid->addSection("Appearance");
        transparency = &appearance->addRow<NumberField>("Transparency", 0.25, 2);
        appearance->addRow<Dropdown>("Material", std::vector<String>{"Plastic", "Wood"}, 0);
        surface.layout();
    }
    PropertyRow &row(PropertySection &section, std::size_t index) { return *section.rows()[index]; }
    void paint() {
        Image image = Image::create(320, 600, AlphaMode::Premultiplied).value();
        RasterPaintBackend backend(image);
        Painter painter(backend);
        surface.paint(painter);
    }
};

void layout() {
    Panel p;
    checkEqual(p.grid->sections().size(), std::size_t(2), "two sections");
    PropertyRow &first = p.row(*p.data, 0);
    check(first.rect().y > p.data->rect().y, "rows below the section header");
    checkEqual(first.editor().rect().x, 120.0f, "editors start at the name column");
    check(p.appearance->rect().y >= p.row(*p.data, 2).rect().bottom(), "sections follow each other");
    p.paint();

    p.grid->setLabelWidth(150);
    p.surface.layout();
    checkEqual(first.editor().rect().x, 150.0f, "every row follows the name column");
}

void collapsing() {
    Panel p;
    const float before = p.appearance->rect().y;
    const Vec2 header{50, p.data->rect().y + 5};
    p.surface.dispatch(pointer(PointerEvent::Type::Move, header));
    p.surface.dispatch(pointer(PointerEvent::Type::Press, header));
    p.surface.dispatch(pointer(PointerEvent::Type::Release, header));
    p.surface.layout();
    check(!p.data->isExpanded(), "a click on the header collapses it");
    check(p.appearance->rect().y < before, "the next section moves up");
    check(p.row(*p.data, 0).rect().isEmpty(), "collapsed rows take no space");
    check(p.surface.hitTest({200, p.data->rect().bottom() - 1}) != &p.row(*p.data, 0).editor(),
          "and are not hit");
    p.paint();
    p.data->setExpanded(true);
    p.surface.layout();
    checkEqual(p.appearance->rect().y, before, "expanded again");
}

void filtering() {
    Panel p;
    p.grid->setFilter("  trans ");
    p.surface.layout();
    check(!p.data->isVisible(), "a section without a match hides");
    check(p.appearance->isVisible(), "the matching section shows");
    check(p.row(*p.appearance, 0).isVisible() && !p.row(*p.appearance, 1).isVisible(), "only the matching row");
    p.grid->setFilter("COLL");
    p.surface.layout();
    check(p.data->isVisible() && p.row(*p.data, 2).isVisible() && !p.row(*p.data, 0).isVisible(),
          "case does not matter");
    p.grid->setFilter("");
    p.surface.layout();
    check(p.data->isVisible() && p.row(*p.data, 0).isVisible(), "no filter shows everything");
}

void scrubbing() {
    Panel p;
    PropertyRow &row = p.row(*p.appearance, 0);
    float total = 0.0f;
    int finished = 0;
    row.setScrub([&total](float pixels) { total += pixels; }, [&finished] { ++finished; });
    const Vec2 label{30, row.rect().center().y};
    p.surface.dispatch(pointer(PointerEvent::Type::Move, label));
    checkEqual(p.surface.cursor(), Cursor::SizeHorizontal, "a scrub cursor over the name");
    p.surface.dispatch(pointer(PointerEvent::Type::Press, label));
    p.surface.dispatch(pointer(PointerEvent::Type::Move, label + Vec2{10, 0}));
    p.surface.dispatch(pointer(PointerEvent::Type::Move, label + Vec2{25, 3}));
    checkNear(total, 25.0f, 1e-5f, "the scrub gets the horizontal movement");
    p.surface.dispatch(pointer(PointerEvent::Type::Move, label + Vec2{35, 3}, Modifier::Shift));
    checkNear(total, 26.0f, 1e-5f, "Shift makes it finer");
    checkEqual(finished, 0, "not finished while dragging");
    p.surface.dispatch(pointer(PointerEvent::Type::Release, label + Vec2{35, 3}));
    checkEqual(finished, 1, "finished once on release");

    // Without a scrub, clicking a name edits its value.
    const Vec2 nameLabel{30, p.row(*p.data, 0).rect().center().y};
    p.surface.dispatch(pointer(PointerEvent::Type::Move, nameLabel));
    p.surface.dispatch(pointer(PointerEvent::Type::Press, nameLabel));
    p.surface.dispatch(pointer(PointerEvent::Type::Release, nameLabel));
    check(p.surface.focus() == p.name, "clicking Name focuses its field");
}

void dividing() {
    Panel p;
    const Vec2 divider{120, p.row(*p.data, 1).rect().center().y};
    p.surface.dispatch(pointer(PointerEvent::Type::Move, divider - Vec2{2, 0}));
    checkEqual(p.surface.cursor(), Cursor::SizeHorizontal, "a resize cursor at the divider");
    p.surface.dispatch(pointer(PointerEvent::Type::Press, divider - Vec2{2, 0}));
    p.surface.dispatch(pointer(PointerEvent::Type::Move, {170, divider.y}));
    p.surface.dispatch(pointer(PointerEvent::Type::Release, {170, divider.y}));
    checkNear(p.grid->labelWidth(), 170.0f, 1e-5f, "dragging it moves the name column");
    p.grid->setLabelWidth(10);
    checkNear(p.grid->labelWidth(), 60.0f, 1e-5f, "never narrower than 60");
    p.grid->clear();
    p.surface.layout();
    checkEqual(p.grid->sections().size(), std::size_t(0), "cleared");
}

} // namespace

int main() {
    layout();
    collapsing();
    filtering();
    scrubbing();
    dividing();
    return cfw::test::finish("PropertyGridTest");
}
