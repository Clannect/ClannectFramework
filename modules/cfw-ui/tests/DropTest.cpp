// Files dragged over a surface: the element under the pointer, or the
// nearest ancestor that takes files, gets Enter, Moves and the Drop; moving
// to another element sends Leave to the old one; refusing everywhere
// refuses the drag; a modal dialog keeps drops from what is behind it; a
// removed target is forgotten. And paintOverlay, which draws a drop
// highlight over an element's children.

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/test/Check.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Dialog.h"
#include "cfw/ui/Surface.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

// An area that records the drag and takes files when `accepts`.
class Well : public Element {
public:
    explicit Well(bool accepts, Vec2 size = {}) : accepts(accepts) {
        if (size.x > 0) {
            setFixedSize(size);
        }
    }
    bool onDrop(const DropEvent &event) override {
        log.push_back(event.type);
        if (event.type == DropEvent::Type::Drop) {
            dropped = event.paths;
        }
        return accepts;
    }
    bool accepts;
    std::vector<DropEvent::Type> log;
    std::vector<String> dropped;
};

DropEvent drag(DropEvent::Type type, float x, float y, std::vector<String> paths = {}) {
    DropEvent e;
    e.type = type;
    e.position = {x, y};
    e.paths = std::move(paths);
    return e;
}

using T = DropEvent::Type;

// Paints a colour: behind its children with paint, over them with
// paintOverlay (a highlight's top strip).
class Swatch : public Element {
public:
    Swatch(Color fill, std::optional<Color> overlay) : fill(fill), overlay(overlay) {}
    void paint(Painter &painter, const Theme &) override { painter.fillRect(rect(), fill); }
    void paintOverlay(Painter &painter, const Theme &) override {
        if (overlay) {
            painter.fillRect({rect().x, rect().y, rect().width, 4.0f}, *overlay);
        }
    }
    Color fill;
    std::optional<Color> overlay;
};

void overlayPaintsOverChildren() {
    Surface surface;
    surface.setSize({40, 40});
    auto &parent = static_cast<Swatch &>(surface.root().add(std::make_unique<Swatch>(Color{0, 0, 1, 1}, Color{1, 0, 0, 1})));
    parent.add<Swatch>(Color{0, 1, 0, 1}, std::nullopt); // covers the parent
    surface.layout();
    Image image = Image::create(40, 40, AlphaMode::Premultiplied).value();
    {
        RasterPaintBackend backend(image);
        Painter painter(backend);
        surface.paint(painter);
    }
    const std::uint8_t *top = image.row(1).data() + 20 * 4;
    const std::uint8_t *middle = image.row(20).data() + 20 * 4;
    check(top[0] == 255 && top[1] == 0, "the overlay is over the child");
    check(middle[0] == 0 && middle[1] == 255, "the child is over the parent's own paint");
}

} // namespace

int main() {
    Surface surface;
    surface.setSize({400, 200});
    auto &row = static_cast<Stack &>(surface.root().add(std::make_unique<Stack>(Stack::Direction::Row, 0.0f)));
    // Left: a container that takes files, holding a child that does not.
    auto &left = row.add<Well>(true, Vec2{200, 200});
    auto &inner = left.add<Well>(false);
    // Right: an area that refuses.
    auto &right = row.add<Well>(false, Vec2{200, 200});
    surface.layout();

    // Over the child: it is asked first, then its parent takes the drag.
    check(surface.dispatch(drag(T::Enter, 50, 50)), "the container takes the drag over its child");
    check(inner.log == std::vector<T>{T::Enter}, "the child was asked first");
    check(left.log == std::vector<T>{T::Enter}, "the container was entered");
    check(surface.dropTarget() == &left, "the container is the target");

    check(surface.dispatch(drag(T::Move, 60, 60)), "moving inside keeps it");
    checkEqual(left.log.back(), T::Move, "the target gets a Move");

    // To the refusing side: the container gets Leave, the drag is refused.
    check(!surface.dispatch(drag(T::Move, 300, 50)), "the refusing area refuses");
    checkEqual(left.log.back(), T::Leave, "leaving the container sends Leave");
    check(surface.dropTarget() == nullptr, "no target");
    checkEqual(right.log.back(), T::Enter, "the refusing area was asked");

    // Back, and drop.
    surface.dispatch(drag(T::Move, 50, 50));
    check(surface.dispatch(drag(T::Drop, 50, 50, {"/tmp/a.png", "/tmp/b.png"})), "the drop is taken");
    check(left.dropped == std::vector<String>{"/tmp/a.png", "/tmp/b.png"}, "with its files");
    check(surface.dropTarget() == nullptr, "the drag is over");

    // A drop without Enter first (WM_DROPFILES) still finds its target.
    left.log.clear();
    check(surface.dispatch(drag(T::Drop, 20, 20, {"/x"})), "a drop without a drag before it");
    check(left.log == std::vector<T>{T::Enter, T::Drop}, "is asked, then dropped");

    // Leaving the window.
    surface.dispatch(drag(T::Enter, 50, 50));
    surface.dispatch(drag(T::Leave, 0, 0));
    checkEqual(left.log.back(), T::Leave, "leaving the window sends Leave");

    // A modal dialog keeps drops from the elements behind it.
    left.log.clear();
    MessageBox::show(surface, "Busy", "Wait", {"OK"});
    surface.layout();
    check(!surface.dispatch(drag(T::Enter, 20, 20)), "nothing behind a modal dialog takes files");
    check(left.log.empty(), "the container is not asked");
    surface.closePopups();
    surface.layout();

    // A removed target is forgotten.
    surface.dispatch(drag(T::Enter, 50, 50));
    check(surface.dropTarget() == &left, "the container is the target again");
    std::unique_ptr<Element> removed = row.remove(left);
    surface.layout();
    check(surface.dropTarget() == nullptr, "a removed target is forgotten");
    surface.dispatch(drag(T::Leave, 0, 0));

    overlayPaintsOverChildren();
    return cfw::test::finish("DropTest");
}
