// How each control describes itself to assistive technology, and the
// actions it offers (Element::describeAccessible, accessibleItems,
// accessibleFocusedItem, accessibleAction).

#include <algorithm>
#include "cfw/core/CharConv.h"
#include <charconv>
#include <cmath>

#include "cfw/core/Strings.h"
#include "cfw/core/Utf8.h"
#include "cfw/ui/Chrome.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Dialog.h"
#include "cfw/ui/PropertyGrid.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Views.h"

namespace cfw {

namespace {

std::u32string decode(StringView utf8) {
    std::u32string out;
    for (std::size_t i = 0; i < utf8.size();) {
        const Utf8Char c = decodeUtf8At(utf8, i);
        out.push_back(c.codepoint);
        i += c.length;
    }
    return out;
}

// "&File" -> "File" ("&&" is a literal ampersand).
String withoutMnemonic(StringView text) {
    String out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '&' && i + 1 < text.size()) {
            ++i;
        }
        out += text[i];
    }
    return out;
}

std::optional<double> parseNumber(StringView text) {
    const String trimmed(trim(text));
    double value = 0.0;
    const auto result = fromChars(trimmed.data(), trimmed.data() + trimmed.size(), value);
    if (result.ec != std::errc() || result.ptr != trimmed.data() + trimmed.size()) {
        return std::nullopt;
    }
    return value;
}

String formatNumber(double value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    return String(buffer, result.ptr);
}

} // namespace

// ---- Label, Button, CheckBox ----------------------------------------------------------

void Label::describeAccessible(AccessibleNode &node) const {
    Element::describeAccessible(node);
    if (node.name.empty()) {
        node.name = m_text;
    }
}

void Button::describeAccessible(AccessibleNode &node) const {
    Element::describeAccessible(node);
    if (node.name.empty()) {
        // An icon button is named by its tooltip.
        node.name = !m_text.empty() ? m_text : toolTip();
        if (node.description == node.name) {
            node.description.clear();
        }
    }
    node.states.checkable = m_checkable;
    node.states.checked = m_checkable && m_checked;
    node.states.isDefault = m_primary;
}

bool Button::accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) {
    if (action == AccessibleAction::Default && !item) {
        activate();
        return true;
    }
    return Element::accessibleAction(action, item, value);
}

void CheckBox::describeAccessible(AccessibleNode &node) const {
    Element::describeAccessible(node);
    if (node.name.empty()) {
        node.name = m_text;
    }
    node.states.checkable = true;
    node.states.checked = m_checked && !m_partial;
    node.states.mixed = m_partial;
}

bool CheckBox::accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) {
    if (action == AccessibleAction::Default && !item) {
        toggle();
        return true;
    }
    return Element::accessibleAction(action, item, value);
}

// ---- Text and numbers --------------------------------------------------------------

void TextField::describeAccessible(AccessibleNode &node) const {
    Element::describeAccessible(node);
    if (node.name.empty()) {
        node.name = m_placeholder;
    }
    // A password's characters are never exposed.
    node.value = m_masked ? String() : text();
    node.states.editable = !m_readOnly;
    node.states.readOnly = m_readOnly;
    node.states.protectedText = m_masked;
}

bool TextField::accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) {
    if (item) {
        return false;
    }
    if (action == AccessibleAction::SetValue && !m_readOnly) {
        // As if typed over everything: one undo step, textChanged.
        m_anchor = 0;
        m_caret = m_text.size();
        replaceSelection(decode(value));
        if (!hasFocus()) {
            // Not typed into, so no focus loss will finish the edit.
            m_dirty = false;
            editingFinished.emit();
        }
        return true;
    }
    if (action == AccessibleAction::Default) {
        return Element::accessibleAction(AccessibleAction::Focus, item, value);
    }
    return Element::accessibleAction(action, item, value);
}

void NumberField::describeAccessible(AccessibleNode &node) const {
    TextField::describeAccessible(node);
    node.role = Role::SpinButton;
    if (m_minimum > -1e299) {
        node.minimum = m_minimum;
    }
    if (m_maximum < 1e299) {
        node.maximum = m_maximum;
    }
    node.current = m_value;
}

bool NumberField::accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) {
    if (item) {
        return false;
    }
    switch (action) {
    case AccessibleAction::Increment:
        commit(m_value + m_step);
        return true;
    case AccessibleAction::Decrement:
        commit(m_value - m_step);
        return true;
    case AccessibleAction::SetValue:
        if (const std::optional<double> number = parseNumber(value)) {
            commit(*number);
            return true;
        }
        return false;
    default:
        return TextField::accessibleAction(action, item, value);
    }
}

void Slider::describeAccessible(AccessibleNode &node) const {
    Element::describeAccessible(node);
    node.minimum = m_minimum;
    node.maximum = m_maximum;
    node.current = m_value;
    node.value = formatNumber(m_value);
}

bool Slider::accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) {
    if (item) {
        return false;
    }
    const double step = m_step > 0.0 ? m_step : (m_maximum - m_minimum) / 100.0;
    switch (action) {
    case AccessibleAction::Increment:
        change(m_value + step);
        return true;
    case AccessibleAction::Decrement:
        change(m_value - step);
        return true;
    case AccessibleAction::SetValue:
        if (const std::optional<double> number = parseNumber(value)) {
            change(*number);
            return true;
        }
        return false;
    default:
        return Element::accessibleAction(action, item, value);
    }
}

void ProgressBar::describeAccessible(AccessibleNode &node) const {
    Element::describeAccessible(node);
    if (!m_busy) {
        node.minimum = 0.0;
        node.maximum = 100.0;
        node.current = std::round(double(m_value) * 100.0);
        node.value = formatNumber(*node.current) + "%";
    }
}

// ---- Choices ----------------------------------------------------------------------------

void Dropdown::describeAccessible(AccessibleNode &node) const {
    Element::describeAccessible(node);
    node.value = currentText();
    node.states.hasPopup = true;
    node.states.expandable = true;
    node.states.expanded = isOpen();
}

bool Dropdown::accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) {
    if (item) {
        return false;
    }
    switch (action) {
    case AccessibleAction::Default:
    case AccessibleAction::Expand:
        open();
        return m_popup != nullptr;
    case AccessibleAction::Collapse:
        if (m_popup && m_popupSurface) {
            m_popupSurface->closePopup(*m_popup);
            return true;
        }
        return false;
    case AccessibleAction::SetValue: {
        const auto it = std::find(m_items.begin(), m_items.end(), value);
        if (it == m_items.end()) {
            return false;
        }
        choose(int(it - m_items.begin()));
        return true;
    }
    default:
        return Element::accessibleAction(action, item, value);
    }
}

void MenuItem::describeAccessible(AccessibleNode &node) const {
    Element::describeAccessible(node);
    if (node.name.empty()) {
        node.name = m_text;
    }
    node.shortcut = m_shortcut;
    node.states.checkable = m_checked;
    node.states.checked = m_checked;
    node.states.hasPopup = hasSubmenu();
}

bool MenuItem::accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) {
    if (action == AccessibleAction::Default && !item) {
        if (m_submenu) {
            if (Menu *owner = menu()) {
                owner->openSubmenuOf(*this, true);
            }
        } else {
            activated.emit(); // closes the menu: nothing may touch `this` after
        }
        return true;
    }
    return Element::accessibleAction(action, item, value);
}

// ---- Menu bar -------------------------------------------------------------------------

void MenuBar::describeAccessible(AccessibleNode &node) const { Element::describeAccessible(node); }

void MenuBar::accessibleItems(std::vector<AccessibleNode> &items) const {
    for (std::size_t i = 0; i < m_titles.size(); ++i) {
        AccessibleNode title;
        title.itemKey = i;
        title.role = Role::MenuItem;
        title.name = withoutMnemonic(m_titles[i].text);
        title.bounds = m_titles[i].rect;
        title.states.hasPopup = true;
        title.states.expanded = m_open == std::ptrdiff_t(i);
        title.states.disabled = !isEnabled();
        items.push_back(std::move(title));
    }
}

bool MenuBar::accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) {
    if (item && *item < m_titles.size() &&
        (action == AccessibleAction::Default || action == AccessibleAction::Expand)) {
        openMenu(std::size_t(*item), true);
        return true;
    }
    return Element::accessibleAction(action, item, value);
}

// ---- Tabs -------------------------------------------------------------------------------

void TabBar::describeAccessible(AccessibleNode &node) const { Element::describeAccessible(node); }

void TabBar::accessibleItems(std::vector<AccessibleNode> &items) const {
    const std::vector<float> w = widths();
    float left = rect().x;
    for (std::size_t i = 0; i < m_tabs.size() && i < w.size(); ++i) {
        AccessibleNode tab;
        tab.itemKey = i;
        tab.role = Role::Tab;
        tab.name = m_tabs[i];
        if (i < m_badges.size() && !m_badges[i].text.empty()) {
            tab.description = m_badges[i].text; // "3" errors
        }
        tab.bounds = {left, rect().y, w[i], rect().height};
        tab.states.selectable = true;
        tab.states.selected = int(i) == m_current;
        tab.states.focusable = true;
        tab.states.focused = hasFocus() && int(i) == m_current;
        tab.states.disabled = !isEnabled();
        items.push_back(std::move(tab));
        left += w[i];
    }
}

std::optional<std::uint64_t> TabBar::accessibleFocusedItem() const {
    if (m_current < 0 || m_current >= int(m_tabs.size())) {
        return std::nullopt;
    }
    return std::uint64_t(m_current);
}

bool TabBar::accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) {
    if (item && *item < m_tabs.size() &&
        (action == AccessibleAction::Default || action == AccessibleAction::Select ||
         action == AccessibleAction::Focus)) {
        choose(int(*item));
        if (action == AccessibleAction::Focus && surface()) {
            surface()->setFocus(this);
        }
        return true;
    }
    return Element::accessibleAction(action, item, value);
}

// ---- Tree -------------------------------------------------------------------------------

void TreeView::describeAccessible(AccessibleNode &node) const {
    Element::describeAccessible(node);
    if (!m_columns.empty()) {
        node.role = Role::Table;
    } else if (!m_showsExpanders) {
        node.role = Role::List;
    }
}

// The rows in view (the tree is virtual: rows scrolled away are not
// exposed until they come into view), and the current row wherever it is.
void TreeView::accessibleItems(std::vector<AccessibleNode> &items) const {
    if (m_rows.empty()) {
        return;
    }
    const RectF view = body();
    const float height = rowHeight();
    const std::size_t first = std::size_t(std::max(0.0f, std::floor(m_scroll / height)));
    const std::size_t last = std::min(m_rows.size(), std::size_t(std::ceil((m_scroll + view.height) / height)) + 1);
    const auto describeRow = [&](std::size_t index) {
        const Row &row = m_rows[index];
        AccessibleNode item;
        item.itemKey = row.id;
        item.role = !m_showsExpanders ? Role::ListItem : Role::TreeItem;
        if (m_columns.empty()) {
            item.name = m_model.text(row.id);
        } else {
            for (std::size_t column = 0; column < m_columns.size(); ++column) {
                const String cell = m_model.cellText(row.id, column);
                if (!cell.empty()) {
                    item.name += item.name.empty() ? cell : ", " + cell;
                }
            }
        }
        item.bounds = rowRect(index).intersected(view);
        item.level = int(row.depth) + 1;
        item.states.selectable = true;
        item.states.selected = m_selected.contains(row.id);
        item.states.focusable = true;
        item.states.focused = hasFocus() && row.id == m_current;
        item.states.expandable = m_showsExpanders && row.hasChildren;
        item.states.expanded = item.states.expandable && m_expanded.contains(row.id);
        item.states.disabled = !isEnabled();
        items.push_back(std::move(item));
    };
    bool currentShown = false;
    for (std::size_t i = first; i < last; ++i) {
        describeRow(i);
        currentShown = currentShown || m_rows[i].id == m_current;
    }
    if (!currentShown) {
        if (const auto it = m_rowIndex.find(m_current); it != m_rowIndex.end()) {
            describeRow(it->second);
        }
    }
}

std::optional<std::uint64_t> TreeView::accessibleFocusedItem() const {
    if (m_current == TreeModel::kRoot || !m_rowIndex.contains(m_current)) {
        return std::nullopt;
    }
    return m_current;
}

bool TreeView::accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) {
    if (!item) {
        return Element::accessibleAction(action, item, value);
    }
    const auto it = m_rowIndex.find(*item);
    if (it == m_rowIndex.end()) {
        return false;
    }
    const std::size_t row = it->second;
    const TreeModel::Id id = *item;
    switch (action) {
    case AccessibleAction::Default:
        activated.emit(id);
        return true;
    case AccessibleAction::Select:
        setCurrentRow(row, false, false);
        return true;
    case AccessibleAction::Focus:
        if (surface()) {
            surface()->setFocus(this);
        }
        setCurrentRow(row, false, false);
        return true;
    case AccessibleAction::Expand:
    case AccessibleAction::Collapse:
        if (!m_rows[row].hasChildren) {
            return false;
        }
        setExpanded(id, action == AccessibleAction::Expand);
        return true;
    case AccessibleAction::ShowMenu:
        if (!m_selected.contains(id)) {
            setCurrentRow(row, false, false);
        }
        contextMenuRequested.emit(id, rowRect(row).center());
        return true;
    default:
        return false;
    }
}

// ---- Property sections, dialogs, colours ------------------------------------------------

void PropertySection::describeAccessible(AccessibleNode &node) const {
    Element::describeAccessible(node);
    if (node.name.empty()) {
        node.name = m_title;
    }
    node.states.expandable = true;
    node.states.expanded = m_expanded;
}

bool PropertySection::accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) {
    if (!item && (action == AccessibleAction::Expand || action == AccessibleAction::Collapse ||
                  action == AccessibleAction::Default)) {
        setExpanded(action == AccessibleAction::Default ? !m_expanded : action == AccessibleAction::Expand);
        return true;
    }
    return Element::accessibleAction(action, item, value);
}

void Dialog::describeAccessible(AccessibleNode &node) const {
    Element::describeAccessible(node);
    if (node.name.empty()) {
        node.name = m_title;
    }
    node.states.modal = true;
}

void ColorSwatch::describeAccessible(AccessibleNode &node) const {
    Element::describeAccessible(node);
    if (node.name.empty()) {
        node.name = m_title;
    }
    node.value = m_color.toHex();
    node.states.hasPopup = true;
}

bool ColorSwatch::accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) {
    if (item) {
        return false;
    }
    if (action == AccessibleAction::Default) {
        openPicker();
        return true;
    }
    if (action == AccessibleAction::SetValue) {
        if (const std::optional<Color> color = Color::fromHex(value)) {
            setColor(*color);
            colorChanged.emit(*color);
            return true;
        }
        return false;
    }
    return Element::accessibleAction(action, item, value);
}

} // namespace cfw
