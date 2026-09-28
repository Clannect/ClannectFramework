#pragma once

// The application shell around the content: panels, toolbars, the menu bar,
// sliders and progress bars.

#include <functional>
#include <vector>

#include "cfw/core/Signal.h"
#include "cfw/text/TextLayout.h"
#include "cfw/ui/Controls.h"

namespace cfw {

// A Stack on the panel colour, with an optional 1px border on one edge (a
// toolbar's bottom, a status bar's top).
class Panel : public Stack {
public:
    enum class Border : std::uint8_t { None, Top, Bottom, Left, Right, All };
    explicit Panel(Direction direction = Direction::Row, float spacing = -1.0f, float padding = 0.0f);
    void setBorder(Border border);
    // The window colour instead of the panel colour.
    void setRecessed(bool recessed);

    void paint(Painter &painter, const Theme &theme) override;

private:
    Border m_border = Border::None;
    bool m_recessed = false;
};

// A thin line between groups: vertical in a row, horizontal in a column.
class Separator : public Element {
public:
    explicit Separator(Stack::Direction along = Stack::Direction::Row);
    void paint(Painter &painter, const Theme &theme) override;

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    Stack::Direction m_along;
};

// A row of flat icon buttons with tooltips, grouped by separators.
class ToolBar : public Panel {
public:
    ToolBar();
    Button &addButton(Icon icon, String toolTip, std::function<void()> action);
    // A checkable button; `action` gets the new state.
    Button &addToggle(Icon icon, String toolTip, std::function<void(bool)> action, bool checked = false);
    void addSeparator();
    // Takes the spare width (what follows sits at the right end).
    void addSpacer();
};

// The window's menu bar: titles that open menus. Click (or press) a title
// to open its menu; while one is open, moving over another title switches
// to it, and Left/Right in a menu move to the neighbouring one. Menus are
// built each time they open, so their items can reflect the current state.
class MenuBar : public Panel {
public:
    MenuBar();
    // `title` may mark a mnemonic with '&' ("&File"); it is not shown.
    void addMenu(String title, std::function<void(Menu &)> fill);
    [[nodiscard]] std::size_t menuCount() const noexcept { return m_titles.size(); }
    // Opens menu `index` (keyboard: focuses its first item).
    void openMenu(std::size_t index, bool fromKeyboard = false);
    [[nodiscard]] std::ptrdiff_t openIndex() const noexcept { return m_open; }
    void describeAccessible(AccessibleNode &node) const override;
    void accessibleItems(std::vector<AccessibleNode> &items) const override;
    bool accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) override;

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    void onHoverChanged(bool) override { invalidatePaint(); }

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    struct Title {
        String text;
        std::function<void(Menu &)> fill;
        TextLayout layout;
        RectF rect;
    };
    [[nodiscard]] std::ptrdiff_t titleAt(Vec2 point) const;
    void layoutTitles();
    std::vector<Title> m_titles;
    std::ptrdiff_t m_open = -1;
    std::ptrdiff_t m_hot = -1;
    Menu *m_menu = nullptr;
};

// A horizontal slider over [minimum, maximum]: drag the thumb, click the
// track to jump there, Left/Right (Shift: 10 steps), Home/End, wheel.
class Slider : public Element {
public:
    explicit Slider(double minimum = 0.0, double maximum = 1.0, double value = 0.0);
    void setRange(double minimum, double maximum);
    void setStep(double step) noexcept { m_step = step; }
    void setValue(double value); // does not emit
    [[nodiscard]] double value() const noexcept { return m_value; }
    [[nodiscard]] double minimum() const noexcept { return m_minimum; }
    [[nodiscard]] double maximum() const noexcept { return m_maximum; }
    Signal<double> valueChanged;
    void describeAccessible(AccessibleNode &node) const override;
    bool accessibleAction(AccessibleAction action, std::optional<std::uint64_t> item, StringView value) override;

    void paint(Painter &painter, const Theme &theme) override;
    bool onPointer(const PointerEvent &event) override;
    bool onKey(const KeyEvent &event) override;
    void onHoverChanged(bool) override { invalidatePaint(); }
    void onFocusChanged(bool) override { invalidatePaint(); }

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    [[nodiscard]] RectF track() const;
    [[nodiscard]] double valueAt(float x) const;
    void change(double value);
    double m_minimum;
    double m_maximum;
    double m_value;
    double m_step = 0.0; // 0: a hundredth of the range
};

// Progress through a task: a fraction in [0, 1], or busy (unknown progress,
// an animated stripe driven by the surface's timers while visible).
class ProgressBar : public Element {
public:
    ProgressBar();
    ~ProgressBar() override;
    void setValue(float fraction);
    [[nodiscard]] float value() const noexcept { return m_value; }
    void setBusy(bool busy);
    [[nodiscard]] bool isBusy() const noexcept { return m_busy; }
    void describeAccessible(AccessibleNode &node) const override;

    void paint(Painter &painter, const Theme &theme) override;

protected:
    Vec2 measureContent(Vec2 available) override;

private:
    void stopAnimation();
    float m_value = 0.0f;
    bool m_busy = false;
    float m_phase = 0.0f;
    std::uint64_t m_timer = 0;
    Surface *m_timerSurface = nullptr;
};

} // namespace cfw
