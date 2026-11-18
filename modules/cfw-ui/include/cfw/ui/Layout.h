#pragma once

// Layout containers beyond Stack: Box (CSS flexbox's everyday subset, with
// an optional painted background, so a card is one element) and Grid
// (columns of equal or weighted width, or as many as fit).
//
//     auto &card = root.add<Box>(Box::Direction::Column, 10.0f);
//     card.setPadding(Insets::all(16));
//     card.setBackground(theme.panel);
//     card.setBorder(theme.border);
//     card.setRadius(theme.radiusLarge);
//     auto &header = card.add<Box>(Box::Direction::Row, 8.0f);
//     header.add<Label>("Storage").setStretch(1);   // takes the spare width
//     header.add<Button>("Manage");
//
// Spare space on the main axis goes to children with a stretch, in
// proportion; when the children do not fit, the stretching ones shrink first
// (down to nothing: a Label with setElide(true) then ends in "…"). The cross
// axis follows align(); Stretch gives every child the full cross size.
//
// Threads: the surface's thread.

#include <cstdint>
#include <optional>
#include <vector>

#include "cfw/core/Color.h"
#include "cfw/core/Signal.h"
#include "cfw/ui/Element.h"

namespace cfw {

struct Insets {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    [[nodiscard]] static constexpr Insets all(float v) noexcept { return {v, v, v, v}; }
    [[nodiscard]] static constexpr Insets symmetric(float horizontal, float vertical) noexcept {
        return {horizontal, vertical, horizontal, vertical};
    }
    [[nodiscard]] constexpr float width() const noexcept { return left + right; }
    [[nodiscard]] constexpr float height() const noexcept { return top + bottom; }
    friend bool operator==(const Insets &, const Insets &) = default;
};

class Box : public Element {
public:
    enum class Direction : std::uint8_t { Row, Column };
    enum class Align : std::uint8_t { Start, Center, End, Stretch };
    enum class Justify : std::uint8_t { Start, Center, End, SpaceBetween };
    // Edges that get the border (a toolbar's bottom line, a sidebar's right).
    enum Side : std::uint8_t { SideLeft = 1, SideTop = 2, SideRight = 4, SideBottom = 8, SideAll = 15 };

    explicit Box(Direction direction = Direction::Column, float gap = 0.0f);

    // ---- Layout ----
    void setDirection(Direction direction);
    [[nodiscard]] Direction direction() const noexcept { return m_direction; }
    void setGap(float gap);
    // Between wrapped lines (default: the gap).
    void setLineGap(float gap);
    void setPadding(Insets padding);
    void setPadding(float padding) { setPadding(Insets::all(padding)); }
    [[nodiscard]] const Insets &padding() const noexcept { return m_padding; }
    void setAlign(Align align);
    void setJustify(Justify justify);
    // Rows only: children that do not fit start a new line (stretch is then
    // ignored).
    void setWrap(bool wrap);
    // The content is never wider than this (0: no limit) and is centred in
    // a wider box (a page's max-width).
    void setMaxContentWidth(float width);
    // The smallest size the box measures (0: none), per axis.
    void setMinimumSize(Vec2 size);

    // ---- Decoration (nothing by default: a plain layout box) ----
    void setBackground(std::optional<Color> color);
    // Shown while the pointer is within the box / while it is pressed.
    void setHoverBackground(std::optional<Color> color);
    void setPressedBackground(std::optional<Color> color);
    void setBorder(std::optional<Color> color, float width = 1.0f, std::uint8_t sides = SideAll);
    // Border colour while the pointer is within the box.
    void setHoverBorder(std::optional<Color> color);
    void setRadius(float radius);
    [[nodiscard]] float radius() const noexcept { return m_radius; }

    // ---- Interaction ----
    // A clickable box (a card, a list row): focusable, a hand pointer, a
    // button role; clicked on a left press and release inside it, or Space
    // and Enter; doubleClicked on the second press of a double click;
    // contextMenuRequested on a right press (surface coordinates).
    void setClickable(bool clickable);
    [[nodiscard]] bool isClickable() const noexcept { return m_clickable; }
    Signal<> clicked;
    Signal<> doubleClicked;
    Signal<Vec2> contextMenuRequested;
    // The press that came with the last `clicked` (its modifiers: Ctrl or
    // Shift for multiple selection).
    [[nodiscard]] Modifier lastClickModifiers() const noexcept { return m_lastModifiers; }

    void paint(Painter &painter, const Theme &theme) override;
    void paintOverlay(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;
    void onHoverWithinChanged(bool within) override;
    void onPressedChanged(bool pressed) override;
    void onFocusChanged(bool focused) override;

protected:
    Vec2 measureContent(Vec2 available) override;
    void arrangeContent(const RectF &rect) override;

private:
    struct Line {
        std::size_t first = 0;
        std::size_t count = 0;
        float main = 0.0f;  // the children's main sizes and gaps
        float cross = 0.0f; // the tallest (row) or widest (column) child
    };
    // Main-axis sizes for the visible children laid out in `innerMain`.
    void resolveMain(float innerMain, float innerCross, std::vector<float> &mains);
    [[nodiscard]] RectF contentRect(const RectF &rect) const;
    [[nodiscard]] std::vector<Element *> visibleChildren() const;
    [[nodiscard]] Vec2 measureWrapped(Vec2 inner, std::vector<Line> *lines);

    Direction m_direction;
    float m_gap;
    std::optional<float> m_lineGap;
    Insets m_padding;
    Align m_align = Align::Stretch;
    Justify m_justify = Justify::Start;
    bool m_wrap = false;
    float m_maxContentWidth = 0.0f;
    Vec2 m_minimum;
    std::optional<Color> m_background;
    std::optional<Color> m_hoverBackground;
    std::optional<Color> m_pressedBackground;
    std::optional<Color> m_border;
    std::optional<Color> m_hoverBorder;
    float m_borderWidth = 1.0f;
    std::uint8_t m_borderSides = SideAll;
    float m_radius = 0.0f;
    bool m_clickable = false;
    Modifier m_lastModifiers = Modifier::None;
};

// Children in rows of columns, row-major in child order. Each row is as tall
// as its tallest child and every child fills its cell.
class Grid : public Element {
public:
    explicit Grid(int columns = 2, float gap = 0.0f);
    // A fixed number of equal columns.
    void setColumns(int columns);
    // Columns sized by weight (CSS `2fr 1fr`): their count is the list's.
    void setColumnWeights(std::vector<float> weights);
    // As many equal columns as fit at least `width` each (CSS repeat(auto-
    // fill, minmax(width, 1fr))); 0 goes back to the fixed count.
    void setMinColumnWidth(float width);
    void setGap(float columnGap, float rowGap);
    void setGap(float gap) { setGap(gap, gap); }
    void setPadding(Insets padding);
    // The column count for a content width (what layout uses).
    [[nodiscard]] std::size_t columnsFor(float width) const;

protected:
    Vec2 measureContent(Vec2 available) override;
    void arrangeContent(const RectF &rect) override;

private:
    [[nodiscard]] std::vector<float> columnWidths(float width) const;
    [[nodiscard]] std::vector<Element *> visibleChildren() const;
    int m_columns;
    std::vector<float> m_weights;
    float m_minColumnWidth = 0.0f;
    float m_columnGap;
    float m_rowGap;
    Insets m_padding;
};

} // namespace cfw
