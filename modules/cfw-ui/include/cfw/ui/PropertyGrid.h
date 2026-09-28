#pragma once

// A property grid (the Properties panel): collapsible sections of rows,
// each a name and an editor element, in a scrolling column. The names share
// one column whose divider can be dragged; a filter shows the rows whose
// name contains it; dragging a row's name "scrubs" it (a number that follows
// the pointer, as in Studio and Blender).

#include <functional>
#include <memory>
#include <vector>

#include "cfw/core/Signal.h"
#include "cfw/text/TextLayout.h"
#include "cfw/ui/Controls.h"

namespace cfw {

class PropertyGrid;

class PropertyRow : public Element {
public:
    PropertyRow(String name, std::unique_ptr<Element> editor);
    [[nodiscard]] const String &name() const noexcept { return m_name; }
    [[nodiscard]] Element &editor() const noexcept { return *m_editor; }
    // Scrubbing: while the name is dragged, `step` gets the horizontal
    // movement since the last call (pixels; Shift makes it finer, a tenth);
    // `finished` runs once on release (one undo step for the whole drag).
    void setScrub(std::function<void(float pixels)> step, std::function<void()> finished = {});
    [[nodiscard]] bool isScrubbable() const noexcept { return static_cast<bool>(m_scrub); }

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    [[nodiscard]] std::optional<Cursor> cursorAt(Vec2 position) const override;

protected:
    Vec2 measureContent(Vec2 available) override;
    void arrangeContent(const RectF &rect) override;

private:
    [[nodiscard]] float labelWidth() const;
    [[nodiscard]] bool onLabel(Vec2 position) const;
    String m_name;
    Element *m_editor = nullptr;
    TextLayout m_layout;
    std::function<void(float)> m_scrub;
    std::function<void()> m_scrubFinished;
    std::optional<float> m_scrubFrom; // the pointer's last x while scrubbing
};

class PropertySection : public Element {
public:
    explicit PropertySection(String title);
    [[nodiscard]] const String &title() const noexcept { return m_title; }
    PropertyRow &addRow(String name, std::unique_ptr<Element> editor);
    template <class T, class... Args> T &addRow(String name, Args &&...args) {
        return static_cast<T &>(addRow(std::move(name), std::make_unique<T>(std::forward<Args>(args)...)).editor());
    }
    [[nodiscard]] std::vector<PropertyRow *> rows() const;
    void setExpanded(bool expanded);
    [[nodiscard]] bool isExpanded() const noexcept { return m_expanded; }
    Signal<bool> expandedChanged;

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;

protected:
    Vec2 measureContent(Vec2 available) override;
    void arrangeContent(const RectF &rect) override;

private:
    friend class PropertyGrid;
    [[nodiscard]] float headerHeight() const;
    String m_title;
    TextLayout m_layout;
    bool m_expanded = true;
};

class PropertyGrid : public ScrollArea {
public:
    PropertyGrid();
    PropertySection &addSection(String title);
    [[nodiscard]] std::vector<PropertySection *> sections() const;
    [[nodiscard]] PropertySection *section(StringView title) const;
    void clear();

    // Rows whose name contains `text` (ignoring case); sections with none
    // are hidden. Empty: everything.
    void setFilter(StringView text);
    [[nodiscard]] const String &filter() const noexcept { return m_filter; }

    // The width of the name column (shared by every row).
    void setLabelWidth(float width);
    [[nodiscard]] float labelWidth() const noexcept { return m_labelWidth; }

    bool onPointer(const PointerEvent &event) override;
    [[nodiscard]] std::optional<Cursor> cursorAt(Vec2 position) const override;

private:
    [[nodiscard]] Stack &column() const;
    [[nodiscard]] bool onDivider(Vec2 position) const;
    void applyFilter();
    String m_filter;
    float m_labelWidth = 120.0f;
    bool m_resizing = false;
};

} // namespace cfw
