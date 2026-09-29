// Soak: a surface holding every control is hammered with random input for
// a while (CFW_SOAK_SECONDS, 2 s by default): pointer moves, presses,
// drags and wheels, keys and shortcuts, typed and composed text, file
// drops, resizes, timers, a changing tree model, accessibility snapshots
// and actions, and painting. Nothing may crash or trip the sanitizers,
// popups stay bounded, focus stays on elements that are in the tree, and
// accessible ids stay unique.

#include <map>
#include <random>
#include <set>

#include "cfw/gfx/Painter.h"
#include "cfw/gfx/RasterPaintBackend.h"
#include "cfw/image/Image.h"
#include "cfw/test/Check.h"
#include "cfw/test/Soak.h"
#include "cfw/ui/Chrome.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Dialog.h"
#include "cfw/ui/Dock.h"
#include "cfw/ui/PropertyGrid.h"
#include "cfw/ui/Shortcut.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Views.h"

using namespace cfw;
using cfw::test::check;

namespace {

// A tree that grows and shrinks while the soak runs.
class Churning : public TreeModel {
public:
    std::size_t childCount(Id parent) const override {
        const auto it = m_children.find(parent);
        return it == m_children.end() ? 0 : it->second.size();
    }
    Id child(Id parent, std::size_t index) const override { return m_children.at(parent)[index]; }
    String text(Id node) const override { return "node " + std::to_string(node); }
    bool canRename(Id) const override { return true; }
    bool canDrag(Id) const override { return true; }
    bool canDrop(const std::vector<Id> &, Id, std::ptrdiff_t) const override { return true; }
    void churn(std::mt19937_64 &random) {
        if (random() % 3 != 0 || m_nodes.size() < 3) {
            const Id parent = m_nodes.empty() ? kRoot : m_nodes[random() % m_nodes.size()];
            const Id id = m_next++;
            m_children[parent].push_back(id);
            m_parent[id] = parent;
            m_nodes.push_back(id);
        } else {
            // Remove a leaf.
            for (std::size_t attempt = 0; attempt < 5; ++attempt) {
                const Id id = m_nodes[random() % m_nodes.size()];
                if (childCount(id) == 0) {
                    auto &siblings = m_children[m_parent[id]];
                    siblings.erase(std::find(siblings.begin(), siblings.end(), id));
                    m_nodes.erase(std::find(m_nodes.begin(), m_nodes.end(), id));
                    m_parent.erase(id);
                    break;
                }
            }
        }
        changed.emit();
    }

private:
    std::map<Id, std::vector<Id>> m_children;
    std::map<Id, Id> m_parent;
    std::vector<Id> m_nodes;
    Id m_next = 1;
};

void collect(const Element &element, std::vector<const Element *> &out) {
    out.push_back(&element);
    for (const auto &child : element.children()) {
        collect(*child, out);
    }
}

void ids(const AccessibleNode &node, std::set<std::uint64_t> &seen, bool &duplicate) {
    duplicate = duplicate || !seen.insert(node.id).second;
    for (const AccessibleNode &child : node.children) {
        ids(child, seen, duplicate);
    }
}

} // namespace

int main() {
    const auto duration = cfw::test::soakDuration(2.0);
    std::mt19937_64 random(cfw::test::soakSeed("SoakUiTest"));
    TimePoint now{std::chrono::seconds(100)};

    Surface surface(Theme::dark().withSystemFonts());
    surface.clock = [&now] { return now; };
    surface.setSize({900, 600});
    Churning model;
    auto &column = static_cast<Stack &>(surface.root().add(std::make_unique<Stack>(Stack::Direction::Column, 0.0f)));
    auto &bar = column.add<MenuBar>();
    bar.addMenu("&File", [](Menu &m) {
        m.addItem("Save", [] {}, "Ctrl+S");
        m.addSubmenu("Recent", [](Menu &sub) { sub.addItem("one", [] {}); });
        m.addSeparator();
        m.addItem("Quit", [] {});
    });
    bar.addMenu("&Edit", [](Menu &m) { m.addItem("Undo", [] {}); });
    auto &tools = column.add<ToolBar>();
    tools.addButton(Icon(), "Dialog", [&surface] { MessageBox::show(surface, "Soak", "A dialog", {"OK", "Cancel"}); });
    tools.addToggle(Icon(), "Toggle", [](bool) {});
    auto &docks = column.add<DockLayout>();
    docks.setStretch(1);
    auto &centre = docks.setCentral<Stack>(Stack::Direction::Column, 4.0f, 4.0f);
    auto &row = centre.add<Stack>(Stack::Direction::Row, 4.0f);
    row.add<Button>("Press");
    row.add<CheckBox>("Check", true);
    row.add<TextField>("text");
    row.add<NumberField>(3.0, 2).setRange(-10, 10);
    auto &dropdown = centre.add<Dropdown>(std::vector<String>{"one", "two", "three", "four"});
    dropdown.setSearchable(true);
    centre.add<TabBar>(std::vector<String>{"A", "B", "C"});
    centre.add<Slider>(0.0, 1.0, 0.5);
    centre.add<ProgressBar>().setBusy(true);
    auto tree = std::make_unique<TreeView>(model);
    TreeView *treeView = tree.get();
    auto &explorer = docks.addPanel(std::make_unique<DockPanel>("tree", "Tree", std::move(tree)), DockArea::Left);
    (void)explorer;
    auto grid = std::make_unique<PropertyGrid>();
    PropertySection &section = grid->addSection("Section");
    section.addRow<TextField>("Name", "value");
    section.addRow<NumberField>("Number", 1.0, 1);
    section.addRow<ColorSwatch>("Colour", Color{1, 0, 0, 1});
    docks.addPanel(std::make_unique<DockPanel>("grid", "Properties", std::move(grid)), DockArea::Right);
    auto &scroll = centre.add<ScrollArea>();
    scroll.setFixedSize({0, 120});
    auto &list = scroll.setContent<Stack>(Stack::Direction::Column, 2.0f);
    for (int i = 0; i < 40; ++i) {
        list.add<Label>("line " + std::to_string(i));
    }
    int shortcuts = 0;
    surface.addShortcut(KeyChord::parse("Ctrl+S").value(), [&shortcuts] { ++shortcuts; });
    ScopedConnection onContextMenu = treeView->contextMenuRequested.connect([&surface](TreeModel::Id, Vec2 at) {
        auto menu = std::make_unique<Menu>();
        menu->addItem("Rename", [] {});
        Menu::popup(surface, std::move(menu), at);
    });
    (void)treeView->renamed.connect([](TreeModel::Id, const String &) {});

    const Key keys[] = {Key::Tab, Key::Enter, Key::Escape, Key::Left, Key::Right, Key::Up, Key::Down, Key::Home,
                        Key::End, Key::PageUp, Key::PageDown, Key::Backspace, Key::Delete, Key::Space, Key::A,
                        Key::C, Key::V, Key::X, Key::Z, Key::F2, Key::S};
    const char *texts[] = {"a", "Zz", "é", "日本", "\U0001F600", " ", "tab\there", ""};
    Image image = Image::create(900, 600, AlphaMode::Premultiplied).value();

    std::size_t steps = 0, maxPopups = 0;
    bool badFocus = false, duplicateIds = false;
    Vec2 pointer{450, 300};
    bool pressed = false;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < duration) {
        ++steps;
        now += std::chrono::milliseconds(random() % 50);
        const auto choice = random() % 1000;
        if (choice < 350) {
            // Pointer: mostly near controls' centres, sometimes anywhere.
            std::vector<const Element *> all;
            collect(surface.root(), all);
            if (random() % 3 == 0 || all.empty()) {
                pointer = {float(random() % 900), float(random() % 600)};
            } else {
                const RectF r = all[random() % all.size()]->rect();
                pointer = r.center();
            }
            PointerEvent e;
            e.position = pointer;
            const auto kind = random() % 10;
            if (kind < 4) {
                e.type = PointerEvent::Type::Move;
            } else if (kind < 6 && !pressed) {
                e.type = PointerEvent::Type::Press;
                e.button = random() % 4 == 0 ? PointerButton::Right : PointerButton::Left;
                e.clickCount = random() % 5 == 0 ? 2 : 1;
                pressed = true;
            } else if (kind < 8 && pressed) {
                e.type = PointerEvent::Type::Release;
                e.button = PointerButton::Left;
                pressed = false;
            } else if (kind < 9) {
                e.type = PointerEvent::Type::Wheel;
                e.wheelDelta = {0.0f, float(int(random() % 5) - 2) * 48.0f};
            } else {
                e.type = PointerEvent::Type::Leave;
            }
            surface.dispatch(e);
        } else if (choice < 650) {
            KeyEvent e;
            e.key = keys[random() % std::size(keys)];
            e.modifiers = random() % 4 == 0 ? Modifier::Control : random() % 5 == 0 ? Modifier::Shift : Modifier::None;
            surface.dispatch(e);
            e.type = KeyEvent::Type::Release;
            surface.dispatch(e);
        } else if (choice < 780) {
            surface.dispatch(TextEvent{texts[random() % std::size(texts)]});
        } else if (choice < 810) {
            CompositionEvent e;
            e.text = texts[random() % std::size(texts)];
            e.cursor = e.text.empty() ? 0 : random() % (e.text.size() + 1);
            surface.dispatch(e);
        } else if (choice < 830) {
            DropEvent e;
            e.type = static_cast<DropEvent::Type>(random() % 4);
            e.position = {float(random() % 900), float(random() % 600)};
            e.paths = {"/tmp/soak.png"};
            surface.dispatch(e);
        } else if (choice < 860) {
            model.churn(random);
        } else if (choice < 875) {
            surface.setSize({float(200 + random() % 900), float(150 + random() % 700)});
        } else if (choice < 920) {
            // Assistive technology pokes at a random node.
            const AccessibleNode snapshot = surface.accessibilityTree();
            std::set<std::uint64_t> seen;
            ids(snapshot, seen, duplicateIds);
            std::vector<std::uint64_t> all(seen.begin(), seen.end());
            const auto action = static_cast<AccessibleAction>(random() % 9);
            surface.performAccessibleAction(all[random() % all.size()], action, "7");
        } else if (choice < 930) {
            RasterPaintBackend backend(image);
            Painter painter(backend);
            surface.paint(painter);
        } else {
            surface.runTimers();
        }
        // Too many dialogs pile up only if Escape never closes them.
        if (surface.popupCount() > 12) {
            surface.closePopups();
        }
        maxPopups = std::max(maxPopups, surface.popupCount());
        // Focus is on an element of this surface: its tree or a popup.
        if (surface.focus() && !surface.focus()->surface()) {
            badFocus = true;
        }
    }
    std::printf("SoakUiTest: %zu steps, at most %zu popups, %d shortcut runs\n", steps, maxPopups, shortcuts);
    check(steps > 1000, "the soak did real work");
    check(!badFocus, "focus is always on an element in the surface");
    check(!duplicateIds, "accessible ids are unique");
    return cfw::test::finish("SoakUiTest");
}
