#pragma once

// On/off and one-of-a-few choices: ToggleSwitch (a settings row's switch)
// and SegmentedControl (aspect ratio 16:9 | 9:16 | 1:1, grid | list).
//
// Both animate their change over Theme-independent 150 ms (a knob sliding,
// the selected segment moving) and jump straight to the end when
// Theme::reducedMotion is set. Keyboard: Space toggles a switch; Left/Right
// (and Home/End) move a segmented control's choice.
//
// Threads: the surface's thread.

#include <memory>
#include <vector>

#include "cfw/core/Signal.h"
#include "cfw/core/String.h"
#include "cfw/text/TextLayout.h"
#include "cfw/ui/Element.h"
#include "cfw/ui/Surface.h"

namespace cfw {

// A sliding on/off switch (34 x 20): the accent colour when on.
class ToggleSwitch : public Element {
public:
    explicit ToggleSwitch(bool checked = false, String accessibleName = {});
    ~ToggleSwitch() override;
    void setChecked(bool checked); // does not emit toggled
    [[nodiscard]] bool isChecked() const noexcept { return m_checked; }
    Signal<bool> toggled; // after a click or Space changed it

    // 0 (off) .. 1 (on) as drawn, mid-animation included.
    [[nodiscard]] float knobPosition() const noexcept { return m_position; }

    void describeAccessible(AccessibleNode &node) const override;
    bool accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) override;
    void paint(Painter &painter, const Theme &theme) override;
    void paintOverlay(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;
    void onHoverChanged(bool) override { invalidatePaint(); }
    void onFocusChanged(bool) override { invalidatePaint(); }

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    void toggle();
    void animateTo(float target);
    bool m_checked;
    float m_position;
    float m_from = 0.0f;
    TimePoint m_animStart{};
    Surface::TimerId m_timer = 0;
};

// A row of mutually exclusive segments in a recessed track; the chosen one
// is raised.
class SegmentedControl : public Element {
public:
    explicit SegmentedControl(std::vector<String> items = {}, int current = 0);
    ~SegmentedControl() override;
    void setItems(std::vector<String> items);
    [[nodiscard]] const std::vector<String> &items() const noexcept { return m_items; }
    void setCurrentIndex(int index); // does not emit currentChanged
    [[nodiscard]] int currentIndex() const noexcept { return m_current; }
    // Segments share the whole width equally (else each is as wide as its
    // text).
    void setFill(bool fill);
    // Segment height (default 26) and text size (default: the theme's - 1).
    void setSegmentHeight(float height);
    void setPixelSize(float size);
    Signal<int> currentChanged;

    // The segment under `position` (surface coordinates), or -1.
    [[nodiscard]] int segmentAt(Vec2 position) const;
    [[nodiscard]] RectF segmentRect(int index) const;

    void describeAccessible(AccessibleNode &node) const override;
    void accessibleItems(std::vector<AccessibleNode> &items) const override;
    [[nodiscard]] std::optional<std::uint64_t> accessibleFocusedItem() const override;
    bool accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) override;
    void paint(Painter &painter, const Theme &theme) override;
    void paintOverlay(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;
    void onHoverChanged(bool) override { invalidatePaint(); }
    void onFocusChanged(bool) override { invalidatePaint(); }

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    static constexpr float kInset = 2.0f; // track padding
    static constexpr float kGap = 2.0f;
    static constexpr float kTextPadding = 10.0f;
    void choose(int index);
    void relayoutText();
    [[nodiscard]] float pixelSize() const;
    std::vector<String> m_items;
    std::vector<TextLayout> m_layouts;
    bool m_layoutsValid = false;
    int m_current;
    bool m_fill = false;
    float m_height = 26.0f;
    float m_pixelSize = 0.0f;
    int m_hoveredSegment = -1;
    // The raised segment's left edge, animated between choices.
    float m_highlightX = -1.0f;
    float m_highlightFrom = 0.0f;
    float m_highlightTo = 0.0f;
    TimePoint m_animStart{};
    Surface::TimerId m_timer = 0;
};

} // namespace cfw
