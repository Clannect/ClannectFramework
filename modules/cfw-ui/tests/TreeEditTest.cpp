// TreeView editing, headless: filtering, rename in place, drag and drop,
// context menus, icons and flat lists.

#include <array>
#include <map>
#include <string>

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/test/Check.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Views.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

using Id = TreeModel::Id;

PointerEvent pointer(PointerEvent::Type type, Vec2 at, PointerButton button = PointerButton::Left) {
    PointerEvent e;
    e.type = type;
    e.position = at;
    e.button = type == PointerEvent::Type::Move ? PointerButton::None : button;
    return e;
}

KeyEvent key(Key k, Modifier modifiers = Modifier::None) {
    KeyEvent e;
    e.key = k;
    e.modifiers = modifiers;
    return e;
}

// A small editable scene: Workspace (1) with Part2, Part3, Model4 (which
// holds Part5); Lighting (6).
class Scene : public TreeModel {
public:
    Scene() {
        add(kRoot, 1, "Workspace");
        add(1, 2, "Part2");
        add(1, 3, "Part3");
        add(1, 4, "Model4");
        add(4, 5, "Part5");
        add(kRoot, 6, "Lighting");
    }
    std::size_t childCount(Id parent) const override {
        const auto it = m_children.find(parent);
        return it == m_children.end() ? 0 : it->second.size();
    }
    Id child(Id parent, std::size_t index) const override { return m_children.at(parent)[index]; }
    String text(Id node) const override { return m_names.at(node); }
    Icon icon(Id node) const override {
        return node == 1 ? Icon::fromSvg(R"(<svg viewBox="0 0 24 24"><rect width="24" height="24" fill="currentColor"/></svg>)")
                         : Icon();
    }
    bool canRename(Id node) const override { return node != 1; }
    bool canDrag(Id node) const override { return node != 1 && node != 6; } // services stay put
    bool canDrop(const std::vector<Id> &nodes, Id parent, std::ptrdiff_t) const override {
        if (parent == kRoot) {
            return false; // only services at the top
        }
        for (const Id node : nodes) {
            for (Id p = parent; p != kRoot; p = m_parents.at(p)) {
                if (p == node) {
                    return false; // not into itself or its own descendant
                }
            }
        }
        return true;
    }

private:
    void add(Id parent, Id id, const char *name) {
        m_children[parent].push_back(id);
        m_parents[id] = parent;
        m_names[id] = name;
    }
    std::map<Id, std::vector<Id>> m_children;
    std::map<Id, Id> m_parents;
    std::map<Id, String> m_names;
};

struct Fixture {
    Scene model;
    Surface surface;
    TreeView *tree = nullptr;
    Fixture() {
        surface.setSize({300, 400});
        tree = &static_cast<TreeView &>(surface.root().add(std::make_unique<TreeView>(model)));
        tree->setExpanded(std::vector<Id>{1, 4}, true);
        surface.layout();
    }
    Vec2 rowCentre(Id node) const { return tree->rowRect(std::size_t(tree->rowOf(node))).center(); }
    void paint() {
        Image image = Image::create(300, 400, AlphaMode::Premultiplied).value();
        RasterPaintBackend backend(image);
        Painter painter(backend);
        surface.paint(painter);
    }
};

void filtering() {
    Fixture f;
    checkEqual(f.tree->rowCount(), std::size_t(6), "everything expanded: six rows");
    f.tree->setFilter([&f](Id id) { return f.model.text(id).find("Part5") != String::npos; });
    checkEqual(f.tree->rowCount(), std::size_t(3), "a match shows with its ancestors");
    checkEqual(f.tree->rowNode(0), Id(1), "Workspace");
    checkEqual(f.tree->rowNode(1), Id(4), "Model4");
    checkEqual(f.tree->rowNode(2), Id(5), "Part5");
    f.tree->setExpanded(4, false);
    checkEqual(f.tree->rowCount(), std::size_t(3), "filtered, everything leading to a match stays open");
    f.tree->setFilter([](Id) { return false; });
    checkEqual(f.tree->rowCount(), std::size_t(0), "no match, no rows");
    f.tree->setFilter({});
    checkEqual(f.tree->rowCount(), std::size_t(5), "no filter: the tree as expanded again");
    f.paint();
}

void renaming() {
    Fixture f;
    std::vector<std::pair<Id, String>> renames;
    ScopedConnection c = f.tree->renamed.connect([&renames](Id id, const String &name) { renames.emplace_back(id, name); });

    f.tree->startRename(1);
    check(!f.tree->isRenaming(), "the model says Workspace cannot be renamed");

    f.tree->startRename(2);
    f.surface.layout();
    check(f.tree->isRenaming(), "renaming Part2");
    auto *field = dynamic_cast<TextField *>(f.surface.focus());
    check(field != nullptr, "the field has the focus");
    checkEqual(field->text(), String("Part2"), "with the current name, selected");
    check(field->rect().y == f.tree->rowRect(1).y, "over its row");
    f.paint();
    f.surface.dispatch(TextEvent{"Door"});
    f.surface.dispatch(key(Key::Enter));
    checkEqual(renames.size(), std::size_t(1), "Enter commits");
    checkEqual(renames[0].second, String("Door"), "the new name");
    check(!f.tree->isRenaming() && f.surface.focus() == f.tree, "the tree has the focus back");

    f.tree->setSelection({3});
    f.surface.dispatch(key(Key::F2));
    check(f.tree->isRenaming(), "F2 renames the current row");
    f.surface.dispatch(TextEvent{"Nope"});
    f.surface.dispatch(key(Key::Escape));
    check(!f.tree->isRenaming(), "Escape cancels");
    checkEqual(renames.size(), std::size_t(1), "without renaming");

    f.tree->startRename(3);
    f.surface.dispatch(TextEvent{"Window"});
    f.surface.dispatch(pointer(PointerEvent::Type::Press, f.rowCentre(6)));
    f.surface.dispatch(pointer(PointerEvent::Type::Release, f.rowCentre(6)));
    checkEqual(renames.size(), std::size_t(2), "pressing elsewhere commits");
    checkEqual(renames[1].second, String("Window"), "the typed name");

    f.tree->startRename(3);
    f.surface.dispatch(key(Key::Enter));
    checkEqual(renames.size(), std::size_t(2), "an unchanged name is no rename");
}

void dragging() {
    Fixture f;
    struct Drop {
        std::vector<Id> nodes;
        Id parent;
        std::ptrdiff_t index;
    };
    std::vector<Drop> drops;
    ScopedConnection c = f.tree->dropped.connect(
        [&drops](const std::vector<Id> &nodes, Id parent, std::ptrdiff_t index) { drops.push_back({nodes, parent, index}); });

    // Part2 onto Model4.
    const Vec2 from = f.rowCentre(2);
    f.surface.dispatch(pointer(PointerEvent::Type::Move, from));
    f.surface.dispatch(pointer(PointerEvent::Type::Press, from));
    f.surface.dispatch(pointer(PointerEvent::Type::Move, from + Vec2{2, 1}));
    f.surface.dispatch(pointer(PointerEvent::Type::Release, from + Vec2{2, 1}));
    checkEqual(drops.size(), std::size_t(0), "a small wobble is a click, not a drag");

    f.surface.dispatch(pointer(PointerEvent::Type::Press, from));
    f.surface.dispatch(pointer(PointerEvent::Type::Move, f.rowCentre(4)));
    f.paint(); // the drop indicator
    f.surface.dispatch(pointer(PointerEvent::Type::Release, f.rowCentre(4)));
    checkEqual(drops.size(), std::size_t(1), "dropped");
    check(drops[0].nodes == std::vector<Id>{2} && drops[0].parent == 4 && drops[0].index == -1, "onto Model4");

    // Between rows: the top quarter of Part3 is "before Part3".
    const RectF part3 = f.tree->rowRect(std::size_t(f.tree->rowOf(3)));
    f.tree->setSelection({5});
    f.surface.dispatch(pointer(PointerEvent::Type::Press, f.rowCentre(5)));
    f.surface.dispatch(pointer(PointerEvent::Type::Move, {part3.center().x, part3.y + 2}));
    f.surface.dispatch(pointer(PointerEvent::Type::Release, {part3.center().x, part3.y + 2}));
    checkEqual(drops.size(), std::size_t(2), "dropped between rows");
    check(drops[1].parent == 1 && drops[1].index == 1, "before Part3, in Workspace at index 1");

    // Nothing drops onto its own descendant, and services do not drag.
    f.tree->setSelection({4});
    f.surface.dispatch(pointer(PointerEvent::Type::Press, f.rowCentre(4)));
    f.surface.dispatch(pointer(PointerEvent::Type::Move, f.rowCentre(5)));
    f.surface.dispatch(pointer(PointerEvent::Type::Release, f.rowCentre(5)));
    checkEqual(drops.size(), std::size_t(2), "not into its own child");
    f.tree->setSelection({6});
    f.surface.dispatch(pointer(PointerEvent::Type::Press, f.rowCentre(6)));
    f.surface.dispatch(pointer(PointerEvent::Type::Move, f.rowCentre(4)));
    f.surface.dispatch(pointer(PointerEvent::Type::Release, f.rowCentre(4)));
    checkEqual(drops.size(), std::size_t(2), "a service does not drag");

    // A multi-selection survives the press so it can be dragged together.
    f.tree->setSelection({2, 3});
    f.surface.dispatch(pointer(PointerEvent::Type::Press, f.rowCentre(2)));
    checkEqual(f.tree->selection().size(), std::size_t(2), "pressing a selected row keeps the selection");
    f.surface.dispatch(pointer(PointerEvent::Type::Move, f.rowCentre(4)));
    f.surface.dispatch(pointer(PointerEvent::Type::Release, f.rowCentre(4)));
    check(drops.size() == 3 && drops[2].nodes == std::vector<Id>({2, 3}), "both rows drop together");
    f.surface.dispatch(pointer(PointerEvent::Type::Press, f.rowCentre(2)));
    f.surface.dispatch(pointer(PointerEvent::Type::Release, f.rowCentre(2)));
    check(f.tree->selection() == std::vector<Id>{2}, "a click without a drag selects just that row");
}

void contextMenus() {
    Fixture f;
    std::vector<Id> asked;
    ScopedConnection c = f.tree->contextMenuRequested.connect([&asked](Id id, Vec2) { asked.push_back(id); });
    f.tree->setSelection({2, 3});
    f.surface.dispatch(pointer(PointerEvent::Type::Press, f.rowCentre(3), PointerButton::Right));
    f.surface.dispatch(pointer(PointerEvent::Type::Release, f.rowCentre(3), PointerButton::Right));
    check(asked == std::vector<Id>{3}, "a right press asks for a menu");
    checkEqual(f.tree->selection().size(), std::size_t(2), "over the selection it keeps it");
    f.surface.dispatch(pointer(PointerEvent::Type::Press, f.rowCentre(6), PointerButton::Right));
    f.surface.dispatch(pointer(PointerEvent::Type::Release, f.rowCentre(6), PointerButton::Right));
    check(f.tree->selection() == std::vector<Id>{6}, "outside it, the row becomes the selection");
    f.surface.dispatch(pointer(PointerEvent::Type::Press, {100, 390}, PointerButton::Right));
    check(asked.size() == 3 && asked[2] == TreeModel::kRoot, "empty space asks with no node");
}

void flatList() {
    Fixture f;
    f.tree->setShowsExpanders(false);
    f.paint();
    check(f.tree->rowCount() == 6, "a list shows the same rows");
}

// A log: flat rows with three columns.
class Log : public TreeModel {
public:
    std::vector<std::array<String, 3>> rows;
    std::size_t childCount(Id parent) const override { return parent == kRoot ? rows.size() : 0; }
    Id child(Id, std::size_t index) const override { return Id(index + 1); }
    String text(Id node) const override { return rows[node - 1][0]; }
    String cellText(Id node, std::size_t column) const override { return rows[node - 1][column]; }
    CellStyle cellStyle(Id node, std::size_t column) const override {
        CellStyle style;
        style.mono = column == 0;
        if (column == 2 && rows[node - 1][1] == "Error") {
            style.color = Color{1, 0.6f, 0.6f, 1};
        }
        return style;
    }
    void append(String time, String type, String message) {
        rows.push_back({std::move(time), std::move(type), std::move(message)});
        changed.emit();
    }
};

void table() {
    Log log;
    Surface surface;
    surface.setSize({400, 200});
    TreeView &view = static_cast<TreeView &>(surface.root().add(std::make_unique<TreeView>(log)));
    view.setShowsExpanders(false);
    view.setColumns({{"Time", 80}, {"Type", 60}, {"Message", 0}});
    for (int i = 0; i < 40; ++i) {
        log.append("12:00:" + std::to_string(10 + i), i % 7 == 0 ? "Error" : "Info", "message " + std::to_string(i));
    }
    surface.layout();

    const float header = view.headerHeight();
    check(header > 0.0f, "titled columns show a header");
    checkEqual(view.rowRect(0).y, header, "rows start below it");
    checkEqual(view.columnWidth(2), 400.0f - 140.0f, "the last column takes the rest");

    // Clicking the header selects nothing; clicking the first row selects it.
    PointerEvent press = pointer(PointerEvent::Type::Press, {200, header / 2.0f});
    surface.dispatch(press);
    surface.dispatch(pointer(PointerEvent::Type::Release, {200, header / 2.0f}));
    check(view.selection().empty(), "the header is not a row");
    surface.dispatch(pointer(PointerEvent::Type::Press, view.rowRect(0).center()));
    surface.dispatch(pointer(PointerEvent::Type::Release, view.rowRect(0).center()));
    surface.dispatch(pointer(PointerEvent::Type::Press, view.rowRect(1).center()));
    PointerEvent shiftClick = pointer(PointerEvent::Type::Press, view.rowRect(1).center());
    surface.dispatch(pointer(PointerEvent::Type::Release, view.rowRect(1).center()));
    shiftClick.modifiers = Modifier::Shift;
    surface.dispatch(pointer(PointerEvent::Type::Press, view.rowRect(0).center()));
    surface.dispatch(pointer(PointerEvent::Type::Release, view.rowRect(0).center()));
    surface.dispatch(shiftClick);
    surface.dispatch(pointer(PointerEvent::Type::Release, view.rowRect(1).center()));
    surface.setFocus(&view);
    surface.dispatch(key(Key::C, Modifier::Control));
    checkEqual(surface.clipboardText(), String("12:00:10\tError\tmessage 0\n12:00:11\tInfo\tmessage 1\n"),
               "Ctrl+C copies the rows, cells separated by tabs");

    // Dragging the border between Time and Type resizes Time.
    surface.dispatch(pointer(PointerEvent::Type::Move, {80, header / 2.0f}));
    checkEqual(surface.cursor(), Cursor::SizeHorizontal, "a resize cursor over a column border");
    surface.dispatch(pointer(PointerEvent::Type::Press, {80, header / 2.0f}));
    surface.dispatch(pointer(PointerEvent::Type::Move, {120, header / 2.0f}));
    surface.dispatch(pointer(PointerEvent::Type::Release, {120, header / 2.0f}));
    checkEqual(view.columnWidth(0), 120.0f, "the column follows the border");
    checkEqual(view.columnWidth(2), 400.0f - 180.0f, "and the last column gives way");

    // A log sticks to the bottom as rows arrive.
    check(!view.isAtBottom(), "forty rows do not fit");
    view.scrollToBottom();
    check(view.isAtBottom(), "scrolled to the end");
    const bool follow = view.isAtBottom();
    log.append("12:01:00", "Info", "new");
    if (follow) {
        view.scrollToBottom();
    }
    check(view.isAtBottom(), "and kept there");
    check(view.rowRect(view.rowCount() - 1).bottom() <= 200.5f, "the new row is in view");

    Image image = Image::create(400, 200, AlphaMode::Premultiplied).value();
    RasterPaintBackend backend(image);
    Painter painter(backend);
    surface.paint(painter);
}

} // namespace

int main() {
    table();
    filtering();
    renaming();
    dragging();
    contextMenus();
    flatList();
    return cfw::test::finish("TreeEditTest");
}
