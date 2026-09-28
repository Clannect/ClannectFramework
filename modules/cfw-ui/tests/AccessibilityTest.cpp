// What assistive technology sees: the tree (window, named controls,
// anonymous containers left out, popups after the window's content), roles,
// names, values and states per control, virtual items (tabs, tree rows,
// menu bar titles) with stable ids, actions on elements and items, focus
// changes reported with the item that has the keyboard, and nothing behind
// a modal dialog.

#include <functional>
#include <map>

#include "cfw/test/Check.h"
#include "cfw/ui/Chrome.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Dialog.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Views.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

class Scene : public TreeModel {
public:
    Scene() {
        add(kRoot, 1, "Workspace");
        add(1, 2, "Baseplate");
        add(1, 3, "Tree");
        add(kRoot, 4, "Lighting");
    }
    std::size_t childCount(Id parent) const override {
        const auto it = m_children.find(parent);
        return it == m_children.end() ? 0 : it->second.size();
    }
    Id child(Id parent, std::size_t index) const override { return m_children.at(parent)[index]; }
    String text(Id node) const override { return m_names.at(node); }

private:
    void add(Id parent, Id id, String name) {
        m_children[parent].push_back(id);
        m_names[id] = std::move(name);
    }
    std::map<Id, std::vector<Id>> m_children;
    std::map<Id, String> m_names;
};

// Every node, depth first.
void collect(const AccessibleNode &node, std::vector<const AccessibleNode *> &out) {
    out.push_back(&node);
    for (const AccessibleNode &child : node.children) {
        collect(child, out);
    }
}

const AccessibleNode *byName(const AccessibleNode &root, StringView name, Role role) {
    std::vector<const AccessibleNode *> all;
    collect(root, all);
    for (const AccessibleNode *node : all) {
        if (node->name == name && node->role == role) {
            return node;
        }
    }
    return nullptr;
}

KeyEvent press(Key key) {
    KeyEvent e;
    e.key = key;
    return e;
}

} // namespace

int main() {
    Surface surface(Theme::dark().withSystemFonts());
    surface.setSize({600, 400});
    surface.accessibleTitle = "Clannect Engine";
    auto &column = static_cast<Stack &>(surface.root().add(std::make_unique<Stack>(Stack::Direction::Column, 4.0f)));
    auto &bar = column.add<MenuBar>();
    bar.addMenu("&File", [](Menu &menu) { menu.addItem("Save", [] {}, "Ctrl+S"); });
    bar.addMenu("&Edit", [](Menu &) {});
    auto &row = column.add<Stack>(Stack::Direction::Row, 4.0f);
    int saves = 0;
    Button &save = row.add<Button>("Save");
    static_cast<void>(save.clicked.connect([&] { ++saves; }));
    CheckBox &snap = row.add<CheckBox>("Snap", true);
    TextField &name = row.add<TextField>("Part");
    name.setAccessibleName("Name");
    TextField &token = row.add<TextField>("secret");
    token.setMasked(true);
    token.setPlaceholder("Access token");
    NumberField &size = row.add<NumberField>(4.0, 1);
    size.setRange(0.0, 10.0);
    size.setStep(0.5);
    size.setAccessibleName("Size");
    Dropdown &material = column.add<Dropdown>(std::vector<String>{"Plastic", "Wood", "Metal"}, 1);
    material.setAccessibleName("Material");
    TabBar &tabs = column.add<TabBar>(std::vector<String>{"Output", "Errors"});
    Scene scene;
    TreeView &tree = column.add<TreeView>(scene);
    tree.setAccessibleName("Explorer");
    tree.setFixedSize({0, 120});
    surface.layout();

    AccessibleNode root = surface.accessibilityTree();
    checkEqual(root.id, std::uint64_t(1), "the window is node 1");
    checkEqual(root.role, Role::Window, "a window");
    checkEqual(root.name, String("Clannect Engine"), "named by the title");
    // The column and row stacks are anonymous: their children are the window's.
    std::vector<const AccessibleNode *> all;
    collect(root, all);
    check(std::none_of(all.begin(), all.end(), [](const AccessibleNode *n) { return n->role == Role::None; }),
          "anonymous containers are left out");
    const AccessibleNode *saveNode = byName(root, "Save", Role::Button);
    check(saveNode && root.parentOf(saveNode->id) == &root, "a button in stacks is a child of the window");
    check(saveNode && saveNode->bounds.width > 0.0f, "with its bounds");
    const std::uint64_t saveId = saveNode ? saveNode->id : 0; // snapshots do not outlive the next one

    // Roles, names, values and states.
    const AccessibleNode *snapNode = byName(root, "Snap", Role::CheckBox);
    check(snapNode && snapNode->states.checkable && snapNode->states.checked, "a checked check box");
    const AccessibleNode *nameNode = byName(root, "Name", Role::TextField);
    check(nameNode && nameNode->value == "Part" && nameNode->states.editable, "a text field's value");
    const AccessibleNode *tokenNode = byName(root, "Access token", Role::TextField);
    check(tokenNode && tokenNode->value.empty() && tokenNode->states.protectedText,
          "a password is named by its placeholder and never shows its value");
    const AccessibleNode *sizeNode = byName(root, "Size", Role::SpinButton);
    check(sizeNode && sizeNode->current == 4.0 && sizeNode->minimum == 0.0 && sizeNode->maximum == 10.0,
          "a number field has its range");
    const AccessibleNode *materialNode = byName(root, "Material", Role::ComboBox);
    check(materialNode && materialNode->value == "Wood" && materialNode->states.hasPopup, "a dropdown's value");
    const AccessibleNode *menuBar = nullptr;
    for (const AccessibleNode *n : all) {
        menuBar = n->role == Role::MenuBar ? n : menuBar;
    }
    check(menuBar && menuBar->children.size() == 2 && menuBar->children[0].name == "File",
          "menu bar titles are items, without their '&'");
    const AccessibleNode *errorsTab = byName(root, "Errors", Role::Tab);
    check(errorsTab && !errorsTab->states.selected, "tabs are items");
    const AccessibleNode *explorer = byName(root, "Explorer", Role::Tree);
    check(explorer && explorer->children.size() == 2, "a tree shows its rows (collapsed: two)");
    const AccessibleNode *workspace = byName(root, "Workspace", Role::TreeItem);
    check(workspace && workspace->states.expandable && !workspace->states.expanded && workspace->level == 1,
          "a collapsed tree item with children");

    // Actions on elements.
    check(surface.performAccessibleAction(saveId, AccessibleAction::Default), "press a button");
    checkEqual(saves, 1, "it clicks");
    check(surface.performAccessibleAction(snapNode->id, AccessibleAction::Default), "toggle a check box");
    check(!snap.isChecked(), "it toggles");
    check(surface.performAccessibleAction(nameNode->id, AccessibleAction::SetValue, "Wall"), "set a text");
    checkEqual(name.text(), String("Wall"), "the text changes");
    check(surface.performAccessibleAction(sizeNode->id, AccessibleAction::Increment), "increment a number");
    checkEqual(size.value(), 4.5, "by its step");
    check(surface.performAccessibleAction(materialNode->id, AccessibleAction::SetValue, "Metal"), "choose an item");
    checkEqual(material.currentIndex(), 2, "the dropdown changes");
    check(!surface.performAccessibleAction(987654321, AccessibleAction::Default), "an unknown node does nothing");

    // Actions on items: ids stay the same between snapshots.
    const std::uint64_t workspaceId = workspace->id;
    check(surface.performAccessibleAction(workspaceId, AccessibleAction::Expand), "expand a tree item");
    root = surface.accessibilityTree();
    workspace = root.find(workspaceId);
    check(workspace && workspace->name == "Workspace" && workspace->states.expanded, "the same id, now expanded");
    check(byName(root, "Baseplate", Role::TreeItem) != nullptr, "its children appear");
    const AccessibleNode *baseplate = byName(root, "Baseplate", Role::TreeItem);
    check(baseplate && baseplate->level == 2, "one level down");
    check(surface.performAccessibleAction(baseplate->id, AccessibleAction::Select), "select a tree item");
    check(tree.selection() == std::vector<TreeModel::Id>{2}, "the tree selects it");
    const std::uint64_t baseplateId = baseplate->id;
    check(surface.performAccessibleAction(byName(root, "Errors", Role::Tab)->id, AccessibleAction::Select),
          "select a tab");
    checkEqual(tabs.currentIndex(), 1, "the tab changes");

    // Focus: the element, or the item with the keyboard.
    std::vector<std::uint64_t> focusEvents;
    ScopedConnection onFocus = surface.accessibleFocusChanged.connect([&](std::uint64_t id) { focusEvents.push_back(id); });
    surface.setFocus(&save);
    check(!focusEvents.empty() && focusEvents.back() == save.accessibleId(), "focusing a button reports it");
    check(surface.performAccessibleAction(baseplateId, AccessibleAction::Focus), "focus a tree item");
    check(!focusEvents.empty() && focusEvents.back() == baseplateId, "the item has the keyboard");
    surface.dispatch(press(Key::Down));
    root = surface.accessibilityTree();
    const AccessibleNode *treeItem = byName(root, "Tree", Role::TreeItem);
    check(treeItem && !focusEvents.empty() && focusEvents.back() == treeItem->id,
          "moving down in the tree reports the next item");
    check(treeItem && treeItem->states.focused, "which is focused in the tree");

    // A modal dialog: in the tree after the window's content, and nothing
    // behind it can be acted on.
    MessageBox::show(surface, "Unsaved Changes", "Save?", {"Save", "Cancel"});
    root = surface.accessibilityTree();
    const AccessibleNode &last = root.children.back();
    check(last.role == Role::Dialog && last.name == "Unsaved Changes" && last.states.modal, "the dialog is last");
    check(byName(last, "Cancel", Role::Button) != nullptr, "with its buttons");
    check(!surface.performAccessibleAction(saveId, AccessibleAction::Default), "nothing behind it acts");
    checkEqual(saves, 1, "the button behind was not pressed");
    check(surface.performAccessibleAction(byName(last, "Cancel", Role::Button)->id, AccessibleAction::Default),
          "the dialog's button presses");
    root = surface.accessibilityTree();
    check(root.children.back().role != Role::Dialog, "and the dialog is gone");

    return cfw::test::finish("AccessibilityTest");
}
