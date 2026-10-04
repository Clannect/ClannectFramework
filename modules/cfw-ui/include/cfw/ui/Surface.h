#pragma once

// A surface hosts one element tree: a window's contents, a popup, or an
// offscreen canvas in tests. It owns the root, the theme and the size, and it
//
// - routes pointer input: hit-testing, hover, press capture (the pressed
//   element gets every pointer event until release), bubbling of unhandled
//   events to parents;
// - routes keys and text to the focused element (bubbling), and moves focus
//   with Tab / Shift+Tab in tree order;
// - lays the tree out when something invalidated it, and collects damage.
//
// Platform windows (cfw-platform) feed it events and paint it each frame
// that has damage; nothing here touches the OS.
//
// Threads: one thread.

#include <map>
#include <functional>
#include <memory>
#include <vector>

#include "cfw/core/Clock.h"
#include "cfw/core/Rect.h"
#include "cfw/core/Signal.h"
#include "cfw/ui/Element.h"
#include "cfw/ui/Shortcut.h"
#include "cfw/ui/Theme.h"

namespace cfw {

class Surface {
public:
    explicit Surface(Theme theme = Theme::dark());
    ~Surface();
    Surface(const Surface &) = delete;
    Surface &operator=(const Surface &) = delete;

    // The root element; replace it to show different content.
    [[nodiscard]] Element &root() noexcept { return *m_root; }
    void setRoot(std::unique_ptr<Element> root);

    void setSize(Vec2 size);
    [[nodiscard]] Vec2 size() const noexcept { return m_size; }
    void setTheme(Theme theme);
    [[nodiscard]] const Theme &theme() const noexcept { return m_theme; }

    // Lays out if anything asked for it. Called by paint() and by input.
    void layout();
    [[nodiscard]] bool needsLayout() const noexcept { return m_layoutDirty; }
    // Paints the whole tree (after layout()).
    void paint(Painter &painter);
    // What changed since the last call (empty: nothing to repaint).
    [[nodiscard]] RectF takeDamage();

    // Input. Returns whether an element handled it. One pointer at a time:
    // the mouse, or the primary touch standing in for it (Window::pointer).
    // A Cancel ends a press without a click.
    bool dispatch(const PointerEvent &event);
    bool dispatch(const KeyEvent &event);
    bool dispatch(const TextEvent &event);
    // Files dragged over the surface: the element under the point, or the
    // nearest ancestor that takes them, gets the drag. Returns whether it
    // would take them (Enter, Move) or took them (Drop).
    bool dispatch(const DropEvent &event);
    // An input method's composition goes to the focused element.
    bool dispatch(const CompositionEvent &event);
    // Where the focused element edits text (for the input method's candidate
    // window); nothing when no text is being edited.
    [[nodiscard]] std::optional<RectF> textInputArea() const;
    [[nodiscard]] Element *dropTarget() const noexcept { return m_dropTarget; }

    // The deepest visible element under `point`.
    [[nodiscard]] Element *hitTest(Vec2 point);
    [[nodiscard]] Element *hovered() const noexcept { return m_hovered; }
    [[nodiscard]] Element *pressed() const noexcept { return m_pressed; }
    [[nodiscard]] Element *focus() const noexcept { return m_focus; }
    void setFocus(Element *element);
    // Tab order: focusable, enabled, visible elements in tree order.
    void focusNext(bool backwards = false);

    // Popups: menus, dropdown lists and tooltips are layers inside the
    // surface, above the root (not separate OS windows; one decision for all
    // platforms). They are measured at their preferred size, placed at
    // `position` and kept inside the surface, painted and hit-tested above
    // everything else. A press outside every popup closes them all (and is
    // not passed on); Escape closes the topmost. `onClosed` runs when a
    // popup closes for any reason.
    Element &openPopup(std::unique_ptr<Element> popup, Vec2 position, std::function<void()> onClosed = {});
    struct PopupOptions {
        // A dialog: the surface under it is dimmed and gets no input, a press
        // outside does not close it, Tab stays inside it, and its first
        // focusable element takes the focus.
        bool modal = false;
        // Centred in the surface (the position is ignored).
        bool centred = false;
        // False for tooltips: never hit, never focused.
        bool takesInput = true;
    };
    Element &openPopup(std::unique_ptr<Element> popup, Vec2 position, const PopupOptions &options,
                       std::function<void()> onClosed = {});
    void closePopup(Element &popup);
    void closePopups();
    [[nodiscard]] std::size_t popupCount() const noexcept { return m_popups.size(); }

    // ---- Timers ----
    // Timers run on the surface's thread, from runTimers(), which the host
    // calls every time round its loop; nextTimer() says how long it may
    // sleep. The clock is Clock::now() unless a test supplies one.
    //
    // A repeating timer fires on a steady grid (start + interval, start + 2 *
    // interval, ...) for as long as runTimers() keeps up, however late each
    // call is within its period. What it guarantees when it cannot keep up -
    // the host stalled, or the callback takes longer than the interval:
    //
    //   - A timer fires at most once per runTimers(). Periods that passed
    //     meanwhile are dropped, not made up: a 16 ms tick whose callback
    //     takes 40 ms runs once every 40 ms or so, never three times in a row.
    //   - A callback never runs inside itself or inside another timer's.
    //   - After a late firing the next one is due one interval after the
    //     time it should have fired, or at once if that has passed too (the
    //     time runTimers() began is what counts as "now"). So a timer that
    //     is always late fires on every runTimers(), and the host's loop
    //     still returns to its events between two firings: a slow timer
    //     delays input by one callback, it does not starve it.
    //   - Timers are independent: one that is slow makes the others late,
    //     but each still fires once on the next runTimers() after it is due,
    //     in the order they were started.
    //
    // A callback may start and stop timers, its own included. A timer
    // started from a callback first fires on a later runTimers().
    using TimerId = std::uint64_t;
    TimerId startTimer(Duration delay, std::function<void()> callback, bool repeat = false);
    void stopTimer(TimerId id);
    void runTimers();
    [[nodiscard]] std::optional<TimePoint> nextTimer() const;
    std::function<TimePoint()> clock;
    [[nodiscard]] TimePoint now() const { return clock ? clock() : Clock::now(); }

    // ---- Shortcuts ----
    // A key press that the focused element (and its ancestors) did not take
    // runs the shortcut with its chord, unless a modal popup is open.
    using ShortcutId = std::uint64_t;
    ShortcutId addShortcut(KeyChord chord, std::function<void()> action);
    void removeShortcut(ShortcutId id);

    // The pointer's shape at the last pointer position.
    [[nodiscard]] Cursor cursor();
    // How long the pointer rests before a tooltip shows.
    static constexpr Duration kToolTipDelay = std::chrono::milliseconds(700);
    [[nodiscard]] Element *toolTip() const noexcept { return m_toolTip; }

    // The clipboard, supplied by the platform window (text only for now).
    // Unset, copy and paste stay inside this surface.
    std::function<String()> readClipboard;
    std::function<void(StringView)> writeClipboard;
    [[nodiscard]] String clipboardText() const;
    void setClipboardText(StringView text);

    // Moves the pointer (surface coordinates), supplied by the platform
    // window. Unset, nothing happens.
    std::function<void(Vec2)> movePointer;

    // ---- Accessibility ----
    // The tree assistive technology sees: the window (id 1, named
    // `accessibleTitle`), the element tree with anonymous containers left
    // out, and the open popups and dialogs after it. Hidden elements are not
    // in it. Built on request.
    [[nodiscard]] AccessibleNode accessibilityTree();
    // Performs `action` on the node `id`; false if it is gone or refused.
    bool performAccessibleAction(std::uint64_t id, AccessibleAction action, StringView value = {});
    // The node with the keyboard (an element, or its focused item); 0: none.
    [[nodiscard]] std::uint64_t accessibleFocus();
    // Emitted with the new node when the keyboard moves (only while
    // someone is connected: the check costs nothing otherwise).
    Signal<std::uint64_t> accessibleFocusChanged;
    String accessibleTitle;

private:
    friend class Element;
    friend struct AccessibleFocusWatch;
    void describeTree(Element &element, std::vector<AccessibleNode> &out,
                      std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> &seen);
    std::uint64_t itemId(std::uint64_t element, std::uint64_t key);
    Element *findAccessible(Element &from, std::uint64_t id);
    void checkAccessibleFocus();
    void layoutInvalidated() noexcept { m_layoutDirty = true; }
    void addDamage(const RectF &rect);
    void elementRemoved(Element &element);
    void setHovered(Element *element);
    void paintTree(Painter &painter, Element &element);
    [[nodiscard]] std::ptrdiff_t topModal() const noexcept;
    [[nodiscard]] bool acceptsInput(const Element *element) const noexcept;
    void hideToolTip();
    void scheduleToolTip();

    Theme m_theme;
    std::unique_ptr<Element> m_root;
    Vec2 m_size;
    bool m_layoutDirty = true;
    RectF m_damage;
    Element *m_hovered = nullptr;
    Element *m_pressed = nullptr;
    Element *m_focus = nullptr;
    Element *m_dropTarget = nullptr;
    String m_clipboard;
    struct Popup {
        std::unique_ptr<Element> element;
        Vec2 position;
        std::function<void()> onClosed;
        PopupOptions options;
    };
    std::vector<Popup> m_popups;
    // Accessible ids of items, per (element id, item key).
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> m_itemIds;
    std::uint64_t m_nextItemId = std::uint64_t(1) << 62;
    std::uint64_t m_lastAccessibleFocus = 0;
    // Closed popups live until the next event or layout: a popup is often
    // closed from inside one of its own callbacks.
    std::vector<std::unique_ptr<Element>> m_closed;

    struct Timer {
        TimerId id;
        TimePoint due;
        Duration interval;
        bool repeat;
        std::function<void()> callback;
    };
    std::vector<Timer> m_timers;
    TimerId m_nextTimer = 1;
    struct Shortcut {
        ShortcutId id;
        KeyChord chord;
        std::function<void()> action;
    };
    std::vector<Shortcut> m_shortcuts;
    ShortcutId m_nextShortcut = 1;

    Vec2 m_pointer;
    bool m_pointerInside = false;
    Element *m_toolTip = nullptr;    // the open tooltip popup
    Element *m_toolTipFor = nullptr; // the element it describes
    TimerId m_toolTipTimer = 0;
    TimePoint m_toolTipHidden{};     // for the quick fall-through to a neighbour
};

} // namespace cfw
