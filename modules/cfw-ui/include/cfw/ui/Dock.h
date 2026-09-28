#pragma once

// Dockable panels around a central element (QMainWindow's docks): panels
// sit in the left, right or bottom area, stacked when an area holds several;
// areas and panels are resized by dragging the gaps between them; a panel is
// moved to another area by dragging its title bar; panels are closed and
// shown again; the arrangement is saved as text and restored.
//
// As in Qt's default, the bottom area spans the whole width under the
// central element and the side areas.

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "cfw/core/Signal.h"
#include "cfw/text/TextLayout.h"
#include "cfw/ui/Chrome.h"
#include "cfw/ui/Element.h"

namespace cfw {

enum class DockArea : std::uint8_t { Left, Right, Bottom };

class DockLayout;

// A panel: a title bar (title, the owner's action buttons, a close button)
// over its content. The title bar may be hidden when the content has its
// own header (the Output panel's tabs).
class DockPanel : public Element {
public:
    DockPanel(String id, String title, std::unique_ptr<Element> content);
    [[nodiscard]] const String &id() const noexcept { return m_id; }
    [[nodiscard]] const String &title() const noexcept { return m_title; }
    [[nodiscard]] Element &content() const noexcept { return *m_content; }
    // Action buttons in the title bar, before the close button.
    Button &addAction(Icon icon, String toolTip, std::function<void()> action);
    void setTitleBarVisible(bool visible);
    [[nodiscard]] bool isTitleBarVisible() const noexcept { return m_titleBarVisible; }
    void setClosable(bool closable);
    // The smallest size it may be given along its area's resizing axis.
    void setMinimumExtent(float extent) noexcept { m_minimum = extent; }
    [[nodiscard]] float minimumExtent() const noexcept { return m_minimum; }
    [[nodiscard]] float titleBarHeight() const;

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;

protected:
    Vec2 measureContent(Vec2 available) override;
    void arrangeContent(const RectF &rect) override;

private:
    friend class DockLayout;
    [[nodiscard]] DockLayout *layout() const;
    String m_id;
    String m_title;
    Element *m_content = nullptr;
    Stack *m_actions = nullptr;
    Button *m_close = nullptr;
    TextLayout m_titleLayout;
    bool m_titleBarVisible = true;
    float m_minimum = 80.0f;
    std::optional<Vec2> m_pressAt;
};

class DockLayout : public Element {
public:
    DockLayout();
    Element &setCentral(std::unique_ptr<Element> central);
    template <class T, class... Args> T &setCentral(Args &&...args) {
        return static_cast<T &>(setCentral(std::make_unique<T>(std::forward<Args>(args)...)));
    }
    [[nodiscard]] Element *central() const noexcept { return m_central; }

    // Adds a panel at the end of `area`.
    DockPanel &addPanel(std::unique_ptr<DockPanel> panel, DockArea area);
    [[nodiscard]] DockPanel *panel(StringView id) const;
    [[nodiscard]] std::vector<DockPanel *> panels(DockArea area) const;
    [[nodiscard]] std::optional<DockArea> areaOf(const DockPanel &panel) const;

    void setPanelVisible(DockPanel &panel, bool visible);
    [[nodiscard]] bool isPanelVisible(const DockPanel &panel) const { return panel.isVisible(); }
    // Moves a panel to `area`, before the panel now at `index` (end if past it).
    void movePanel(DockPanel &panel, DockArea area, std::size_t index = SIZE_MAX);

    // The side areas' widths and the bottom area's height.
    void setAreaSize(DockArea area, float size);
    [[nodiscard]] float areaSize(DockArea area) const;
    // A panel's share of its area (panels in one area split it by share).
    void setPanelShare(DockPanel &panel, float share);

    // "id:area:share:visible;..." with the area sizes first; restoreState
    // ignores unknown panels and leaves panels it does not mention alone.
    [[nodiscard]] String saveState() const;
    bool restoreState(StringView state);

    Signal<DockPanel &, bool> visibilityChanged;

    static constexpr float kGap = 5.0f;

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    [[nodiscard]] std::optional<Cursor> cursorAt(Vec2 position) const override;

protected:
    void arrangeContent(const RectF &rect) override;

private:
    friend class DockPanel;
    struct Entry {
        DockPanel *panel;
        DockArea area;
        float share = 1.0f;
    };
    // What a drag on a gap resizes.
    struct Handle {
        RectF rect;
        std::optional<DockArea> area;     // resizing an area
        std::ptrdiff_t first = -1;        // or the boundary between two panels
        std::ptrdiff_t second = -1;
        bool vertical = false;            // the gap runs up and down (resize along x)
    };
    [[nodiscard]] std::vector<std::size_t> visibleIn(DockArea area) const;
    [[nodiscard]] std::optional<Handle> handleAt(Vec2 position) const;
    [[nodiscard]] std::optional<DockArea> dropAreaAt(Vec2 position) const;
    [[nodiscard]] RectF areaRect(DockArea area) const;
    [[nodiscard]] std::ptrdiff_t entryOf(const DockPanel &panel) const;
    void beginPanelDrag(DockPanel &panel);
    void updatePanelDrag(Vec2 position);
    void endPanelDrag(Vec2 position);

    Element *m_central = nullptr;
    std::vector<Entry> m_entries; // in order within each area
    float m_sizes[3] = {280.0f, 300.0f, 220.0f};
    // Rectangles from the last arrange.
    RectF m_areaRects[3];
    RectF m_centralRect;

    std::optional<Handle> m_resizing;
    Vec2 m_resizeStart;
    float m_resizeFrom = 0.0f;
    float m_resizeFromSecond = 0.0f;

    DockPanel *m_dragging = nullptr;
    std::optional<DockArea> m_dropArea;
    Element *m_indicator = nullptr; // the drop target popup
};

} // namespace cfw
