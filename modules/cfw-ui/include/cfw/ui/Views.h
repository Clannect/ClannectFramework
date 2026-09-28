#pragma once

// Splitter, TabBar and TreeView.

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cfw/core/Signal.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Element.h"
#include "cfw/ui/Icon.h"

namespace cfw {

// Two panes side by side (Row) or stacked (Column) with a draggable divider.
// The ratio is the first pane's share; minimum sizes are kept.
class Splitter : public Element {
public:
    explicit Splitter(Stack::Direction direction = Stack::Direction::Row, float ratio = 0.5f);
    Element &setFirst(std::unique_ptr<Element> pane);
    Element &setSecond(std::unique_ptr<Element> pane);
    void setRatio(float ratio);
    [[nodiscard]] float ratio() const noexcept { return m_ratio; }
    void setMinimumPaneSize(float size) noexcept { m_minimum = size; }
    Signal<float> ratioChanged;

    static constexpr float kHandle = 5.0f;

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    void onHoverChanged(bool) override { invalidatePaint(); }
    [[nodiscard]] std::optional<Cursor> cursorAt(Vec2 position) const override;

protected:
    void arrangeContent(const RectF &rect) override;

private:
    [[nodiscard]] RectF handle() const;
    Element *pane(std::size_t index) const;
    Stack::Direction m_direction;
    float m_ratio;
    float m_minimum = 40.0f;
    Element *m_first = nullptr;
    Element *m_second = nullptr;
    bool m_dragging = false;
};

// A row of tabs; the current one is highlighted. Click or Left/Right.
class TabBar : public Element {
public:
    explicit TabBar(std::vector<String> tabs = {});
    void setTabs(std::vector<String> tabs);
    [[nodiscard]] const std::vector<String> &tabs() const noexcept { return m_tabs; }
    void setCurrentIndex(int index); // does not emit
    [[nodiscard]] int currentIndex() const noexcept { return m_current; }
    Signal<int> currentChanged;

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;
    void onHoverChanged(bool) override { invalidatePaint(); }
    void onFocusChanged(bool) override { invalidatePaint(); }

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    [[nodiscard]] std::vector<float> widths() const;
    [[nodiscard]] int tabAt(float x) const;
    void choose(int index);
    std::vector<String> m_tabs;
    int m_current = 0;
    int m_hoveredTab = -1;
};

// What a TreeView shows. Nodes are opaque ids; 0 is the invisible root.
class TreeModel {
public:
    using Id = std::uint64_t;
    static constexpr Id kRoot = 0;
    virtual ~TreeModel() = default;
    [[nodiscard]] virtual std::size_t childCount(Id parent) const = 0;
    [[nodiscard]] virtual Id child(Id parent, std::size_t index) const = 0;
    [[nodiscard]] virtual String text(Id node) const = 0;
    // Shown before the text (none by default).
    [[nodiscard]] virtual Icon icon(Id) const { return {}; }
    // What the view may do with a node (nothing by default).
    [[nodiscard]] virtual bool canRename(Id) const { return false; }
    [[nodiscard]] virtual bool canDrag(Id) const { return false; }
    // Whether `nodes` may be dropped into `parent` at `index` (before the
    // child now at that index), or onto it when index is -1.
    [[nodiscard]] virtual bool canDrop(const std::vector<Id> &, Id /*parent*/, std::ptrdiff_t /*index*/) const {
        return false;
    }
    // The model calls this after any change; the view rebuilds its rows.
    Signal<> changed;
};

// A virtualised tree (the Explorer): only the rows in view are drawn, so a
// scene with 100,000 instances costs what its visible rows cost. Rows are
// the expanded part of the model, rebuilt when the model changes or a node
// is expanded. Selection: click, Ctrl+click toggles, Shift+click extends;
// Up/Down/Home/End/PageUp/PageDown move (Shift extends), Right expands or
// goes to the first child, Left collapses or goes to the parent, Enter or a
// double-click activates, F2 renames. The view scrolls itself (wheel,
// keeping the current row visible).
//
// With a flat model and setShowsExpanders(false) it is a list view.
//
// Editing, where the model allows it:
// - rename in place (F2 or startRename()): Enter or leaving the field
//   commits (renamed), Escape cancels;
// - drag selected rows and drop them before, after or onto another row
//   (dropped: the nodes, the new parent, the index or -1 for "onto");
// - a right press asks for a context menu, over the row's selection.
//
// A filter shows the nodes it matches with their ancestors, all expanded
// (the Explorer's search).
class TreeView : public Element {
public:
    explicit TreeView(TreeModel &model);
    ~TreeView() override;

    void setExpanded(TreeModel::Id node, bool expanded);
    // Many nodes at once, with one rebuild (expand all, restore a saved state).
    void setExpanded(const std::vector<TreeModel::Id> &nodes, bool expanded);
    [[nodiscard]] bool isExpanded(TreeModel::Id node) const { return m_expanded.contains(node); }
    void setSelection(std::vector<TreeModel::Id> selection);
    [[nodiscard]] std::vector<TreeModel::Id> selection() const;
    [[nodiscard]] TreeModel::Id current() const noexcept { return m_current; }
    // Expands the ancestors of `node` (given as a path from the root) and scrolls to it.
    void reveal(const std::vector<TreeModel::Id> &path);
    void setShowsExpanders(bool shows);
    // Null: no filter.
    void setFilter(std::function<bool(TreeModel::Id)> matches);
    [[nodiscard]] bool isFiltered() const noexcept { return static_cast<bool>(m_filter); }

    void startRename(TreeModel::Id node);
    [[nodiscard]] bool isRenaming() const noexcept { return m_renaming != TreeModel::kRoot; }
    void cancelRename();

    [[nodiscard]] float rowHeight() const;
    [[nodiscard]] std::size_t rowCount() const noexcept { return m_rows.size(); }
    [[nodiscard]] TreeModel::Id rowNode(std::size_t row) const { return m_rows[row].id; }
    // The row's rectangle in surface coordinates (may be scrolled out of view).
    [[nodiscard]] RectF rowRect(std::size_t row) const;
    [[nodiscard]] std::ptrdiff_t rowOf(TreeModel::Id id) const;
    [[nodiscard]] float scrollOffset() const noexcept { return m_scroll; }
    void scrollTo(float offset);
    // Rows painted by the last paint(): the virtualisation guarantee.
    [[nodiscard]] std::size_t rowsPainted() const noexcept { return m_painted; }

    Signal<> selectionChanged;
    Signal<TreeModel::Id> activated;
    Signal<TreeModel::Id, const String &> renamed;
    Signal<const std::vector<TreeModel::Id> &, TreeModel::Id, std::ptrdiff_t> dropped;
    // The node under the pointer (kRoot: empty space) and where, in surface coordinates.
    Signal<TreeModel::Id, Vec2> contextMenuRequested;

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;
    void onFocusChanged(bool) override { invalidatePaint(); }

protected:
    Vec2 measureContent(Vec2 available) override;
    void arrangeContent(const RectF &rect) override;

private:
    struct Row {
        TreeModel::Id id;
        TreeModel::Id parent;
        std::uint32_t depth;
        bool hasChildren;
    };
    struct Drop {
        TreeModel::Id parent = TreeModel::kRoot;
        std::ptrdiff_t index = -1;
        std::ptrdiff_t row = -1; // the row it is relative to (-1: below the last)
        enum class Zone : std::uint8_t { Before, Onto, After } zone = Zone::Onto;
    };
    void rebuild();
    void addRows(TreeModel::Id parent, std::uint32_t depth);
    bool computeVisible(TreeModel::Id node);
    void setCurrentRow(std::size_t row, bool extend, bool toggle);
    void ensureRowVisible(std::size_t row);
    [[nodiscard]] float maxScroll() const;
    [[nodiscard]] std::ptrdiff_t rowAt(float y) const;
    [[nodiscard]] float textX(const Row &row) const;
    [[nodiscard]] std::optional<Drop> dropAt(Vec2 position) const;
    [[nodiscard]] std::ptrdiff_t indexInParent(TreeModel::Id parent, TreeModel::Id node) const;
    void commitRename();

    TreeModel &m_model;
    ScopedConnection m_modelChanged;
    std::vector<Row> m_rows;
    std::unordered_map<TreeModel::Id, std::size_t> m_rowIndex;
    std::unordered_set<TreeModel::Id> m_expanded;
    std::set<TreeModel::Id> m_selected;
    TreeModel::Id m_current = TreeModel::kRoot;
    TreeModel::Id m_anchor = TreeModel::kRoot;
    float m_scroll = 0.0f;
    std::size_t m_painted = 0;
    bool m_showsExpanders = true;

    std::function<bool(TreeModel::Id)> m_filter;
    std::unordered_map<TreeModel::Id, bool> m_visible; // filtered: node or a descendant matches

    TextField *m_editor = nullptr;
    TreeModel::Id m_renaming = TreeModel::kRoot;
    std::vector<ScopedConnection> m_editorConnections;

    // Pointer gestures: a press may become a drag.
    Vec2 m_pressAt;
    std::ptrdiff_t m_pressRow = -1;
    bool m_selectOnRelease = false; // a press on a multi-selection keeps it for a drag
    bool m_dragging = false;
    std::optional<Drop> m_drop;
};

} // namespace cfw
