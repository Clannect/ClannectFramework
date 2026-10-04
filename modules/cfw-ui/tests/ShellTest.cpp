// The surface's application-shell services, headless: key chords and
// shortcuts, timers on a fake clock, modal popups, tooltips and cursors.

#include "cfw/test/Check.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Shortcut.h"
#include "cfw/ui/Surface.h"
#include "cfw/ui/Views.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

PointerEvent pointer(PointerEvent::Type type, float x, float y) {
    PointerEvent e;
    e.type = type;
    e.position = {x, y};
    e.button = type == PointerEvent::Type::Move ? PointerButton::None : PointerButton::Left;
    return e;
}

KeyEvent key(Key k, Modifier modifiers = Modifier::None) {
    KeyEvent e;
    e.key = k;
    e.modifiers = modifiers;
    return e;
}

void click(Surface &surface, float x, float y) {
    surface.dispatch(pointer(PointerEvent::Type::Move, x, y));
    surface.dispatch(pointer(PointerEvent::Type::Press, x, y));
    surface.dispatch(pointer(PointerEvent::Type::Release, x, y));
}

// A clock the test moves by hand.
struct FakeClock {
    TimePoint time{std::chrono::seconds(1000)};
    void advance(Duration by) { time += by; }
};

void keyChords() {
    const auto undo = KeyChord::parse("Ctrl+Z");
    check(undo.has_value(), "Ctrl+Z parses");
    checkEqual(undo->key, Key::Z, "its key");
    checkEqual(undo->modifiers, Modifier::Control, "its modifier");
    checkEqual(KeyChord::parse("shift+ctrl+z")->text(), String("Ctrl+Shift+Z"), "modifiers are shown in order");
    checkEqual(KeyChord::parse("F5")->key, Key::F5, "function keys");
    checkEqual(KeyChord::parse("Shift+F11")->text(), String("Shift+F11"), "shift and a function key");
    checkEqual(KeyChord::parse("Ctrl+`")->key, Key::Backquote, "punctuation");
    checkEqual(KeyChord::parse("Delete")->text(), String("Delete"), "a named key");
    checkEqual(KeyChord::parse("Ctrl+1")->key, Key::Digit1, "digits");
    check(!KeyChord::parse("Ctrl+").has_value(), "no key");
    check(!KeyChord::parse("Hyper+A").has_value(), "unknown modifier");
    check(!KeyChord::parse("Banana").has_value(), "unknown key");

    check(undo->matches(key(Key::Z, Modifier::Control)), "the chord matches its press");
    check(!undo->matches(key(Key::Z, Modifier::Control | Modifier::Shift)), "extra modifiers do not match");
    KeyEvent release = key(Key::Z, Modifier::Control);
    release.type = KeyEvent::Type::Release;
    check(!undo->matches(release), "releases do not match");
}

void timers() {
    FakeClock clock;
    Surface surface;
    surface.clock = [&clock] { return clock.time; };

    int once = 0;
    int repeats = 0;
    surface.startTimer(std::chrono::milliseconds(100), [&once] { ++once; });
    const auto repeating =
        surface.startTimer(std::chrono::milliseconds(30), [&repeats] { ++repeats; }, true);
    check(surface.nextTimer() == clock.time + std::chrono::milliseconds(30), "the earliest timer is next");

    surface.runTimers();
    checkEqual(once + repeats, 0, "nothing is due yet");
    clock.advance(std::chrono::milliseconds(30));
    surface.runTimers();
    checkEqual(repeats, 1, "the repeating timer fires");
    clock.advance(std::chrono::milliseconds(70));
    surface.runTimers();
    checkEqual(once, 1, "the one-shot fires");
    checkEqual(repeats, 2, "a late repeat fires once, not for every missed period");
    clock.advance(std::chrono::milliseconds(200));
    surface.runTimers();
    checkEqual(once, 1, "a one-shot fires once");
    surface.stopTimer(repeating);
    clock.advance(std::chrono::seconds(1));
    surface.runTimers();
    checkEqual(repeats, 3, "a stopped timer stays stopped");
    check(!surface.nextTimer().has_value(), "no timers left");

    // A timer that stops another due at the same time, and one that starts a timer.
    int second = 0;
    Surface::TimerId victim = 0;
    surface.startTimer(Duration::zero(), [&] { surface.stopTimer(victim); });
    victim = surface.startTimer(Duration::zero(), [&second] { ++second; });
    surface.startTimer(Duration::zero(), [&] { surface.startTimer(Duration::zero(), [&second] { second += 10; }); });
    surface.runTimers();
    checkEqual(second, 0, "a timer stopped by an earlier callback does not run, a new one waits");
    surface.runTimers();
    checkEqual(second, 10, "the timer started in a callback runs next time");
}

// What a repeating timer does when it cannot keep up (documented in
// Surface.h): the engine runs a 16 ms tick and a 33 ms brush timer here.
void slowTimers() {
    FakeClock clock;
    Surface surface;
    surface.clock = [&clock] { return clock.time; };
    const TimePoint start = clock.time;
    const auto ms = [](int n) { return Duration(std::chrono::milliseconds(n)); };

    int ticks = 0, brushes = 0, depth = 0, deepest = 0;
    std::vector<int> order;
    Duration tickCost = ms(0);
    surface.startTimer(ms(16), [&] {
        deepest = std::max(deepest, ++depth);
        ++ticks;
        order.push_back(16);
        clock.advance(tickCost); // the callback takes this long
        --depth;
    }, true);
    surface.startTimer(ms(33), [&] {
        deepest = std::max(deepest, ++depth);
        ++brushes;
        order.push_back(33);
        --depth;
    }, true);

    // Keeping up, but always a little late: the grid does not drift.
    for (int frame = 1; frame <= 10; ++frame) {
        clock.time = start + ms(16 * frame + 5);
        surface.runTimers();
    }
    checkEqual(ticks, 10, "a timer served a little late each time still fires once per period");
    check(surface.nextTimer() == start + ms(16 * 11), "and stays on its grid: the lateness does not add up");
    checkEqual(brushes, 5, "the 33 ms timer fired for 33, 66, 99, 132 and 165");

    // The host stalls for a second: the missed periods are dropped.
    clock.time = start + ms(1200);
    surface.runTimers();
    checkEqual(ticks, 11, "after a stall a timer fires once, not once per missed period");
    checkEqual(brushes, 6, "each of them");
    check(surface.nextTimer() == clock.time, "a timer that missed its next time too is due at once");

    // The tick's callback takes 40 ms, more than twice its interval.
    tickCost = ms(40);
    ticks = brushes = 0;
    order.clear();
    for (int pass = 0; pass < 5; ++pass) {
        surface.runTimers(); // the host comes straight back: its other work takes no time here
    }
    checkEqual(ticks, 5, "a callback slower than its interval runs once per runTimers()");
    checkEqual(deepest, 1, "and never inside itself or another timer");
    check(surface.nextTimer().has_value() && *surface.nextTimer() <= clock.time,
          "it is due again at once, so the host does not sleep - but it does get its turn");
    // 200 ms passed in those five passes: the brush timer was due in each.
    checkEqual(brushes, 5, "the other timer is made late, but still fires once on each pass it is due");
    check(order == std::vector<int>({16, 33, 16, 33, 16, 33, 16, 33, 16, 33}), "in the order the timers were started");

    // The callback gets fast again. The firing that was late is followed by
    // one more at once (its next time had passed too); then the tick is on a
    // 16 ms grid again, counted from now.
    tickCost = ms(0);
    surface.runTimers();
    check(surface.nextTimer() == clock.time, "the late firing's successor is due at once");
    surface.runTimers();
    check(surface.nextTimer() == clock.time + ms(16), "and after it the next firing is one interval on");
    ticks = 0;
    clock.advance(ms(15));
    surface.runTimers();
    checkEqual(ticks, 0, "not before");
    clock.advance(ms(1));
    surface.runTimers();
    checkEqual(ticks, 1, "then");

    // A repeating timer that stops itself from its own slow callback.
    Surface::TimerId self = 0;
    int runs = 0;
    self = surface.startTimer(ms(10), [&] {
        ++runs;
        clock.advance(ms(50));
        surface.stopTimer(self);
    }, true);
    clock.advance(ms(10));
    surface.runTimers();
    surface.runTimers();
    checkEqual(runs, 1, "a timer stopped from its own callback does not fire again, however late it was");
}

void shortcuts() {
    Surface surface;
    surface.setSize({300, 200});
    auto &column = static_cast<Stack &>(surface.root().add(std::make_unique<Stack>()));
    auto &field = column.add<TextField>("abc");
    int saved = 0;
    int deleted = 0;
    const auto save = surface.addShortcut(*KeyChord::parse("Ctrl+S"), [&saved] { ++saved; });
    surface.addShortcut(*KeyChord::parse("Delete"), [&deleted] { ++deleted; });
    surface.layout();

    surface.dispatch(key(Key::S, Modifier::Control));
    checkEqual(saved, 1, "a shortcut runs with nothing focused");
    surface.setFocus(&field);
    surface.dispatch(key(Key::S, Modifier::Control));
    checkEqual(saved, 2, "and when the focused field does not use the key");
    surface.dispatch(key(Key::Delete));
    checkEqual(deleted, 0, "a key the focused field uses is its own");
    surface.setFocus(nullptr);
    surface.dispatch(key(Key::Delete));
    checkEqual(deleted, 1, "unfocused, Delete is the shortcut's");
    surface.removeShortcut(save);
    check(!surface.dispatch(key(Key::S, Modifier::Control)), "a removed shortcut does nothing");
    checkEqual(saved, 2, "still two saves");
}

void modalPopups() {
    Surface surface;
    surface.setSize({400, 300});
    auto &behind = surface.root().add<Button>("Behind");
    int behindClicks = 0;
    ScopedConnection c1 = behind.clicked.connect([&behindClicks] { ++behindClicks; });
    int shortcutRuns = 0;
    surface.addShortcut(*KeyChord::parse("Ctrl+S"), [&shortcutRuns] { ++shortcutRuns; });
    surface.layout();

    auto dialog = std::make_unique<Stack>(Stack::Direction::Column, 4.0f, 8.0f);
    dialog->setFixedSize({200, 100});
    auto &ok = dialog->add<Button>("OK");
    auto &cancel = dialog->add<Button>("Cancel");
    bool closed = false;
    Surface::PopupOptions options;
    options.modal = true;
    options.centred = true;
    Element &shown = surface.openPopup(std::move(dialog), {}, options, [&closed] { closed = true; });
    surface.layout();

    checkEqual(shown.rect(), RectF{100, 100, 200, 100}, "a centred dialog");
    check(surface.focus() == &ok, "the dialog's first control takes the focus");
    click(surface, 20, 20);
    checkEqual(behindClicks, 0, "the surface under a dialog gets no clicks");
    check(!closed, "a press outside does not close a dialog");
    check(surface.hitTest({20, 20}) == nullptr, "nothing under the dialog is hit");
    surface.dispatch(key(Key::Tab));
    check(surface.focus() == &cancel, "Tab moves inside the dialog");
    surface.dispatch(key(Key::Tab));
    check(surface.focus() == &ok, "and wraps inside it");
    surface.dispatch(key(Key::S, Modifier::Control));
    checkEqual(shortcutRuns, 0, "no shortcuts under a dialog");

    // A menu above the dialog closes on an outside press; the dialog stays.
    auto menu = std::make_unique<Menu>();
    menu->addItem("Item", [] {});
    surface.openPopup(std::move(menu), {110, 110});
    surface.layout();
    checkEqual(surface.popupCount(), std::size_t(2), "a menu over the dialog");
    click(surface, 5, 5);
    checkEqual(surface.popupCount(), std::size_t(1), "the press closed the menu only");

    surface.dispatch(key(Key::Escape));
    check(closed, "Escape closes the dialog");
    surface.layout();
    click(surface, 20, 20);
    checkEqual(behindClicks, 1, "the surface gets clicks again");
    surface.dispatch(key(Key::S, Modifier::Control));
    checkEqual(shortcutRuns, 1, "and shortcuts");
}

void toolTips() {
    FakeClock clock;
    Surface surface;
    surface.clock = [&clock] { return clock.time; };
    surface.setSize({400, 300});
    auto &row = static_cast<Stack &>(surface.root().add(std::make_unique<Stack>(Stack::Direction::Row, 0.0f)));
    auto &save = row.add<Button>("Save");
    save.setFixedSize({100, 30});
    save.setToolTip("Save the place (Ctrl+S)");
    auto &open = row.add<Button>("Open");
    open.setFixedSize({100, 30});
    open.setToolTip("Open a place");
    auto &plain = row.add<Button>("Plain");
    plain.setFixedSize({100, 30});
    surface.layout();

    surface.dispatch(pointer(PointerEvent::Type::Move, 50, 15));
    check(surface.toolTip() == nullptr, "no tooltip at once");
    clock.advance(Surface::kToolTipDelay - std::chrono::milliseconds(1));
    surface.runTimers();
    check(surface.toolTip() == nullptr, "not before the delay");
    clock.advance(std::chrono::milliseconds(1));
    surface.runTimers();
    check(surface.toolTip() != nullptr, "after resting, the tooltip shows");
    surface.layout();
    check(surface.toolTip()->rect().y >= 30.0f, "below the pointer");
    check(surface.hitTest({50, 50}) != surface.toolTip() && surface.hitTest(surface.toolTip()->rect().center()) !=
                                                                surface.toolTip(),
          "a tooltip is never hit");

    surface.dispatch(pointer(PointerEvent::Type::Move, 55, 16));
    check(surface.toolTip() != nullptr, "moving on the same element keeps it");
    surface.dispatch(pointer(PointerEvent::Type::Move, 150, 15));
    check(surface.toolTip() != nullptr, "moving to a neighbour with a tooltip shows its tooltip at once");

    surface.dispatch(pointer(PointerEvent::Type::Move, 250, 15));
    check(surface.toolTip() == nullptr, "an element without one closes it");
    clock.advance(std::chrono::seconds(2));
    surface.runTimers();
    check(surface.toolTip() == nullptr, "and none appears later");

    surface.dispatch(pointer(PointerEvent::Type::Move, 50, 15));
    clock.advance(std::chrono::milliseconds(100));
    surface.runTimers();
    check(surface.toolTip() == nullptr, "after a while away, the delay applies again");
    clock.advance(Surface::kToolTipDelay);
    surface.runTimers();
    check(surface.toolTip() != nullptr, "shown again");
    surface.dispatch(pointer(PointerEvent::Type::Press, 50, 15));
    check(surface.toolTip() == nullptr, "a press hides it");
    surface.dispatch(pointer(PointerEvent::Type::Release, 50, 15));
}

void cursors() {
    Surface surface;
    surface.setSize({400, 100});
    auto &splitter = static_cast<Splitter &>(surface.root().add(std::make_unique<Splitter>(Stack::Direction::Row, 0.5f)));
    splitter.setFirst(std::make_unique<TextField>("text"));
    splitter.setSecond(std::make_unique<Button>("button"));
    surface.layout();

    surface.dispatch(pointer(PointerEvent::Type::Move, 20, 10));
    checkEqual(surface.cursor(), Cursor::IBeam, "an I-beam over a text field");
    const float handle = std::round((400.0f - Splitter::kHandle) * 0.5f) + 2.0f;
    surface.dispatch(pointer(PointerEvent::Type::Move, handle, 50));
    checkEqual(surface.cursor(), Cursor::SizeHorizontal, "a resize cursor over the splitter's handle");
    surface.dispatch(pointer(PointerEvent::Type::Move, 350, 10));
    checkEqual(surface.cursor(), Cursor::Arrow, "an arrow over a button");
    surface.dispatch(pointer(PointerEvent::Type::Leave, 0, 0));
    checkEqual(surface.cursor(), Cursor::Arrow, "an arrow once the pointer left");
}

} // namespace

int main() {
    keyChords();
    timers();
    slowTimers();
    shortcuts();
    modalPopups();
    toolTips();
    cursors();
    return cfw::test::finish("ShellTest");
}
