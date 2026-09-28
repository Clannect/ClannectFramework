#pragma once

// What assistive technology (a screen reader, a switch device, UI tests)
// sees of a surface: a tree of nodes with a role, a name, a value, states,
// bounds and actions. Elements describe themselves (Element::
// describeAccessible) and the items they draw without child elements (tree
// rows, tabs, menu bar titles); the surface builds the tree on request
// (Surface::accessibilityTree) and performs actions on nodes by id. The
// platform bridges (MSAA on Windows, AT-SPI on Linux) translate it.
//
// Threads: the surface's thread.

#include <cstdint>
#include <optional>
#include <vector>

#include "cfw/core/Rect.h"
#include "cfw/core/String.h"

namespace cfw {

// What a node is. Bridges map these to MSAA roles and AT-SPI roles.
enum class Role : std::uint8_t {
    None,        // no meaning of its own: its children stand in its place
    Window,      // the surface's root
    Group,
    Pane,        // a docked panel
    Label,
    Button,
    CheckBox,
    TextField,
    SpinButton,  // a number field
    ComboBox,    // a dropdown
    List,
    ListItem,
    Tree,
    TreeItem,
    Table,       // a tree with columns
    Menu,
    MenuBar,
    MenuItem,
    ToolBar,
    TabList,
    Tab,
    Dialog,
    Slider,
    ProgressBar,
    ScrollArea,
    Separator,
    Image,
};

struct AccessibleStates {
    bool focusable = false;
    bool focused = false;
    bool disabled = false;
    bool checkable = false;
    bool checked = false;
    bool mixed = false;      // a check box between on and off
    bool expandable = false;
    bool expanded = false;
    bool selectable = false;
    bool selected = false;
    bool editable = false;
    bool readOnly = false;
    bool protectedText = false; // a password: the value is never exposed
    bool modal = false;
    bool hasPopup = false;
    bool isDefault = false;  // the button Enter presses

    friend bool operator==(const AccessibleStates &, const AccessibleStates &) = default;
};

enum class AccessibleAction : std::uint8_t {
    Default,   // press a button, toggle a check box, activate an item
    Focus,
    Select,    // select an item
    Expand,
    Collapse,
    Increment,
    Decrement,
    SetValue,  // replace a text or number value
    ShowMenu,  // the context menu
};

struct AccessibleNode {
    std::uint64_t id = 0; // stable while the element (or item) lives; the window is 1
    Role role = Role::None;
    String name;
    String value;
    String description;
    String shortcut;      // "Ctrl+S"
    AccessibleStates states;
    RectF bounds;         // surface coordinates (logical pixels)
    int level = 0;        // depth of a tree item, from 1
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::optional<double> current;
    std::vector<AccessibleNode> children;
    // For items an element draws itself: the element's key for the item,
    // handed back to Element::accessibleAction.
    std::uint64_t itemKey = 0;

    // The node with `nodeId` in this subtree, or null.
    [[nodiscard]] const AccessibleNode *find(std::uint64_t nodeId) const;
    // Its parent in this subtree, or null.
    [[nodiscard]] const AccessibleNode *parentOf(std::uint64_t nodeId) const;
};

// The role's name for people ("push button"), used by bridges without a
// role vocabulary of their own and by tests.
[[nodiscard]] StringView roleName(Role role) noexcept;

} // namespace cfw
