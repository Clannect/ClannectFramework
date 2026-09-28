#include "cfw/ui/Accessibility.h"

namespace cfw {

const AccessibleNode *AccessibleNode::find(std::uint64_t nodeId) const {
    if (id == nodeId) {
        return this;
    }
    for (const AccessibleNode &child : children) {
        if (const AccessibleNode *found = child.find(nodeId)) {
            return found;
        }
    }
    return nullptr;
}

const AccessibleNode *AccessibleNode::parentOf(std::uint64_t nodeId) const {
    for (const AccessibleNode &child : children) {
        if (child.id == nodeId) {
            return this;
        }
        if (const AccessibleNode *found = child.parentOf(nodeId)) {
            return found;
        }
    }
    return nullptr;
}

StringView roleName(Role role) noexcept {
    switch (role) {
    case Role::None: return "unknown";
    case Role::Window: return "window";
    case Role::Group: return "grouping";
    case Role::Pane: return "panel";
    case Role::Label: return "label";
    case Role::Button: return "push button";
    case Role::CheckBox: return "check box";
    case Role::TextField: return "text";
    case Role::SpinButton: return "spin button";
    case Role::ComboBox: return "combo box";
    case Role::List: return "list";
    case Role::ListItem: return "list item";
    case Role::Tree: return "tree";
    case Role::TreeItem: return "tree item";
    case Role::Table: return "tree table";
    case Role::Menu: return "menu";
    case Role::MenuBar: return "menu bar";
    case Role::MenuItem: return "menu item";
    case Role::ToolBar: return "tool bar";
    case Role::TabList: return "page tab list";
    case Role::Tab: return "page tab";
    case Role::Dialog: return "dialog";
    case Role::Slider: return "slider";
    case Role::ProgressBar: return "progress bar";
    case Role::ScrollArea: return "scroll pane";
    case Role::Separator: return "separator";
    case Role::Image: return "image";
    }
    return "unknown";
}

} // namespace cfw
