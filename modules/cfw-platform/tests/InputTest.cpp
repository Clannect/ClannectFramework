// The parts of input that are arithmetic and routing, on every platform and
// without a window system: keys by position from scan codes and evdev codes,
// left and right modifiers, relative mouse deltas from devices that report
// movement and from devices that report positions, and touch contacts routed
// to Window::touch and (the primary one) to Window::pointer.

#include <thread>
#include <vector>

#include "../src/KeyCodes.h"
#include "../src/RelativeMotion.h"
#include "cfw/image/Image.h"
#include "cfw/platform/Window.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

void keysByPosition() {
    using detail::physicalKeyFromEvdev;
    using detail::physicalKeyFromScanCode;
    // The movement cluster, by position: the same scan codes on every layout.
    checkEqual(physicalKeyFromScanCode(0x11, false), Key::W, "scan code 0x11 is the W position");
    checkEqual(physicalKeyFromScanCode(0x1E, false), Key::A, "0x1E the A position (which types Q on AZERTY)");
    checkEqual(physicalKeyFromScanCode(0x1F, false), Key::S, "0x1F");
    checkEqual(physicalKeyFromScanCode(0x20, false), Key::D, "0x20");
    checkEqual(physicalKeyFromScanCode(0x10, false), Key::Q, "0x10 the Q position (which types A on AZERTY)");
    checkEqual(physicalKeyFromScanCode(0x2C, false), Key::Z, "0x2C the Z position (which types Y on QWERTZ)");
    checkEqual(physicalKeyFromScanCode(0x02, false), Key::Digit1, "the digit row starts at 1");
    checkEqual(physicalKeyFromScanCode(0x0B, false), Key::Digit0, "and ends at 0");
    checkEqual(physicalKeyFromScanCode(0x39, false), Key::Space, "space");
    checkEqual(physicalKeyFromScanCode(0x3B, false), Key::F1, "F1");
    checkEqual(physicalKeyFromScanCode(0x44, false), Key::F10, "F10");
    checkEqual(physicalKeyFromScanCode(0x58, false), Key::F12, "F12");
    checkEqual(physicalKeyFromScanCode(0x29, false), Key::Backquote, "the key left of 1");
    checkEqual(physicalKeyFromScanCode(0x35, false), Key::Slash, "slash");

    // Left and right modifiers: told apart by scan code or the extended flag.
    checkEqual(physicalKeyFromScanCode(0x2A, false), Key::LeftShift, "left Shift");
    checkEqual(physicalKeyFromScanCode(0x36, false), Key::RightShift, "right Shift has its own scan code");
    checkEqual(physicalKeyFromScanCode(0x1D, false), Key::LeftControl, "left Control");
    checkEqual(physicalKeyFromScanCode(0x1D, true), Key::RightControl, "right Control is the extended one");
    checkEqual(physicalKeyFromScanCode(0x38, false), Key::LeftAlt, "left Alt");
    checkEqual(physicalKeyFromScanCode(0x38, true), Key::RightAlt, "right Alt (AltGr)");
    checkEqual(physicalKeyFromScanCode(0x5B, true), Key::LeftMeta, "left Windows key");
    checkEqual(physicalKeyFromScanCode(0x5C, true), Key::RightMeta, "right Windows key");
    // The navigation block shares codes with the number pad; the flag decides.
    checkEqual(physicalKeyFromScanCode(0x48, true), Key::Up, "the arrow key");
    checkEqual(physicalKeyFromScanCode(0x48, false), Key::Unknown, "the number pad's 8 is not the arrow key");
    checkEqual(physicalKeyFromScanCode(0x1C, true), Key::Enter, "the number pad's Enter is Enter");
    checkEqual(physicalKeyFromScanCode(0x53, true), Key::Delete, "Delete");
    checkEqual(physicalKeyFromScanCode(0xFF, false), Key::Unknown, "an unknown code");

    // Linux: evdev codes (an X11 key code minus 8).
    checkEqual(physicalKeyFromEvdev(17), Key::W, "KEY_W");
    checkEqual(physicalKeyFromEvdev(30), Key::A, "KEY_A");
    checkEqual(physicalKeyFromEvdev(42), Key::LeftShift, "KEY_LEFTSHIFT");
    checkEqual(physicalKeyFromEvdev(54), Key::RightShift, "KEY_RIGHTSHIFT");
    checkEqual(physicalKeyFromEvdev(29), Key::LeftControl, "KEY_LEFTCTRL");
    checkEqual(physicalKeyFromEvdev(97), Key::RightControl, "KEY_RIGHTCTRL");
    checkEqual(physicalKeyFromEvdev(56), Key::LeftAlt, "KEY_LEFTALT");
    checkEqual(physicalKeyFromEvdev(100), Key::RightAlt, "KEY_RIGHTALT");
    checkEqual(physicalKeyFromEvdev(125), Key::LeftMeta, "KEY_LEFTMETA");
    checkEqual(physicalKeyFromEvdev(126), Key::RightMeta, "KEY_RIGHTMETA");
    checkEqual(physicalKeyFromEvdev(103), Key::Up, "KEY_UP");
    checkEqual(physicalKeyFromEvdev(111), Key::Delete, "KEY_DELETE");
    checkEqual(physicalKeyFromEvdev(87), Key::F11, "KEY_F11");
    checkEqual(physicalKeyFromEvdev(72), Key::Unknown, "KEY_KP8 is not an arrow");
    checkEqual(physicalKeyFromEvdev(240), Key::Unknown, "KEY_UNKNOWN");

    // X11: the names XKB gives keys by position.
    using detail::physicalKeyFromXkbName;
    checkEqual(physicalKeyFromXkbName("AD02"), Key::W, "AD02 is the W position");
    checkEqual(physicalKeyFromXkbName("AC01"), Key::A, "AC01 the A position");
    checkEqual(physicalKeyFromXkbName("AB01"), Key::Z, "AB01 the Z position");
    checkEqual(physicalKeyFromXkbName("AE10"), Key::Digit0, "AE10 is 0");
    checkEqual(physicalKeyFromXkbName("AE12"), Key::Equal, "AE12");
    checkEqual(physicalKeyFromXkbName("AC11"), Key::Quote, "AC11");
    checkEqual(physicalKeyFromXkbName("AB10"), Key::Slash, "AB10");
    checkEqual(physicalKeyFromXkbName("FK11"), Key::F11, "FK11");
    checkEqual(physicalKeyFromXkbName("TLDE"), Key::Backquote, "TLDE");
    checkEqual(physicalKeyFromXkbName("LFSH"), Key::LeftShift, "LFSH");
    checkEqual(physicalKeyFromXkbName("RTSH"), Key::RightShift, "RTSH");
    checkEqual(physicalKeyFromXkbName("RCTL"), Key::RightControl, "RCTL");
    checkEqual(physicalKeyFromXkbName("RALT"), Key::RightAlt, "RALT");
    checkEqual(physicalKeyFromXkbName("LWIN"), Key::LeftMeta, "LWIN");
    checkEqual(physicalKeyFromXkbName("UP"), Key::Up, "UP");
    checkEqual(physicalKeyFromXkbName("ESC"), Key::Escape, "ESC");
    checkEqual(physicalKeyFromXkbName("AC12"), Key::Unknown, "a column the row does not have");
    checkEqual(physicalKeyFromXkbName("AD00"), Key::Unknown, "column 0");
    checkEqual(physicalKeyFromXkbName("KP8"), Key::Unknown, "the number pad");
    checkEqual(physicalKeyFromXkbName(""), Key::Unknown, "no name");

    // The sided keys fold back to the ones existing code compares with.
    checkEqual(eitherSide(Key::LeftShift), Key::Shift, "LeftShift is a Shift");
    checkEqual(eitherSide(Key::RightShift), Key::Shift, "RightShift is a Shift");
    checkEqual(eitherSide(Key::RightControl), Key::Control, "RightControl is a Control");
    checkEqual(eitherSide(Key::LeftAlt), Key::Alt, "LeftAlt is an Alt");
    checkEqual(eitherSide(Key::RightMeta), Key::Meta, "RightMeta is a Meta");
    checkEqual(eitherSide(Key::W), Key::W, "any other key is itself");
    check(isModifierKey(Key::RightAlt) && isModifierKey(Key::Shift) && !isModifierKey(Key::A), "isModifierKey");
    // The existing keys keep their values: new ones were appended.
    checkEqual(int(Key::Meta) + 1, int(Key::LeftShift), "the sided keys follow the old ones");
    KeyEvent old;
    old.key = Key::A;
    checkEqual(old.physicalKey, Key::Unknown, "an event built the old way has no position");
}

void relativeMotion() {
    detail::RelativeMotion motion;
    // A mouse: its counts, as they are, however large (no screen edge).
    check(motion.relative(3, -2) == Vec2{3, -2}, "a relative device's movement is the delta");
    check(motion.relative(5000, 0) == Vec2{5000, 0}, "a fast move is not clamped");
    // A tablet or a remote desktop: positions over a 1920 x 1080 screen.
    const Vec2 screen{1920, 1080};
    check(motion.absolute(32767.5f, 32767.5f, 65535, screen) == Vec2{0, 0}, "the first absolute position is not a movement");
    const Vec2 step = motion.absolute(32767.5f + 65535.0f / 1920.0f * 10.0f, 32767.5f, 65535, screen);
    check(std::abs(step.x - 10.0f) < 0.01f && step.y == 0.0f, "then the difference, in screen pixels");
    const Vec2 back = motion.absolute(32767.5f, 32767.5f - 65535.0f / 1080.0f * 4.0f, 65535, screen);
    check(std::abs(back.x + 10.0f) < 0.01f && std::abs(back.y + 4.0f) < 0.01f, "in both directions");
    motion.reset();
    check(motion.absolute(0, 0, 65535, screen) == Vec2{0, 0}, "after a reset the next position starts afresh");
}

// A window with no window system behind it: only the shared routing.
class PlainWindow final : public Window {
public:
    void show() override {}
    void hide() override {}
    void setTitle(StringView) override {}
    void setSize(Vec2i) override {}
    Vec2i pixelSize() const override { return {640, 480}; }
    float devicePixelRatio() const override { return 1.0f; }
    void present(const Image &) override {}
    Result<Image> capture() const override { return Error(ErrorCode::Unsupported, "no pixels"); }
    void requestRepaint() override {}
    void setCursor(Cursor) override {}
    void *nativeHandle() const override { return nullptr; }
};

PointerEvent contact(PointerEvent::Type type, std::uint32_t id, bool primary, Vec2 at, float pressure = 1.0f) {
    PointerEvent e;
    e.type = type;
    e.kind = PointerKind::Touch;
    e.id = id;
    e.primary = primary;
    e.position = at;
    e.pressure = pressure;
    return e;
}

void touchRouting() {
    PlainWindow window;
    std::vector<PointerEvent> asMouse, touches;
    ScopedConnection c1 = window.pointer.connect([&](const PointerEvent &e) { asMouse.push_back(e); });
    ScopedConnection c2 = window.touch.connect([&](const PointerEvent &e) { touches.push_back(e); });
    using Type = PointerEvent::Type;

    // One finger: exactly what a left click and drag would have produced.
    window.deliverTouch(contact(Type::Press, 7, true, {100, 50}));
    window.deliverTouch(contact(Type::Move, 7, true, {120, 60}));
    window.deliverTouch(contact(Type::Release, 7, true, {120, 60}));
    checkEqual(asMouse.size(), std::size_t(3), "the primary touch reaches mouse-only code");
    check(asMouse[0].type == Type::Press && asMouse[0].button == PointerButton::Left && asMouse[0].clickCount == 1,
          "as a left button press");
    check(asMouse[1].type == Type::Move && asMouse[1].button == PointerButton::None && asMouse[1].position == Vec2{120, 60},
          "a move");
    check(asMouse[2].type == Type::Release && asMouse[2].button == PointerButton::Left, "and a release");
    check(asMouse[0].kind == PointerKind::Touch && asMouse[0].id == 7, "labelled as the touch it is, for code that asks");
    checkEqual(touches.size(), std::size_t(3), "and is on the touch signal too");

    // Two more fingers while the first is down: only the first is the mouse.
    asMouse.clear();
    touches.clear();
    window.deliverTouch(contact(Type::Press, 1, true, {10, 10}));
    window.deliverTouch(contact(Type::Press, 2, false, {200, 10}, 0.4f));
    window.deliverTouch(contact(Type::Press, 3, false, {300, 10}));
    window.deliverTouch(contact(Type::Move, 2, false, {210, 40}, 0.6f));
    window.deliverTouch(contact(Type::Move, 1, true, {12, 30}));
    window.deliverTouch(contact(Type::Release, 3, false, {300, 10}));
    window.deliverTouch(contact(Type::Cancel, 2, false, {210, 40}));
    window.deliverTouch(contact(Type::Release, 1, true, {12, 30}));
    checkEqual(touches.size(), std::size_t(8), "every finger's every event is on the touch signal");
    checkEqual(asMouse.size(), std::size_t(3), "only the primary finger's are on the pointer signal");
    check(asMouse[0].id == 1 && asMouse[1].id == 1 && asMouse[2].id == 1, "the same finger throughout");
    check(touches[1].id == 2 && !touches[1].primary && touches[1].pressure == 0.4f, "ids, primary and pressure are carried");
    check(touches[3].type == Type::Move && touches[3].id == 2 && touches[3].position == Vec2{210, 40},
          "a second finger moves on its own");
    check(touches[6].type == Type::Cancel && touches[6].id == 2, "a cancelled contact says so");

    // A double tap is a double click; a tap elsewhere or later is not.
    asMouse.clear();
    window.deliverTouch(contact(Type::Press, 4, true, {50, 50}));
    window.deliverTouch(contact(Type::Release, 4, true, {50, 50}));
    window.deliverTouch(contact(Type::Press, 5, true, {54, 47}));
    window.deliverTouch(contact(Type::Release, 5, true, {54, 47}));
    window.deliverTouch(contact(Type::Press, 6, true, {300, 300}));
    checkEqual(asMouse[0].clickCount, 1, "the first tap");
    checkEqual(asMouse[2].clickCount, 2, "a second tap nearby and soon is a double click");
    checkEqual(asMouse[4].clickCount, 1, "a tap elsewhere starts again");
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    window.deliverTouch(contact(Type::Press, 8, true, {300, 300}));
    checkEqual(asMouse[5].clickCount, 1, "and so does a tap too late");

    // A cancelled primary touch reaches the pointer signal as Cancel.
    asMouse.clear();
    window.deliverTouch(contact(Type::Press, 9, true, {5, 5}));
    window.deliverTouch(contact(Type::Cancel, 9, true, {5, 5}));
    check(asMouse.size() == 2 && asMouse[1].type == Type::Cancel, "a cancelled primary touch is a Cancel, never a Release");

    // A pen: hovering moves have no pressure, contact has the pen's.
    touches.clear();
    PointerEvent pen = contact(Type::Move, 20, true, {40, 40}, 0.0f);
    pen.kind = PointerKind::Pen;
    window.deliverTouch(pen);
    pen.type = Type::Press;
    pen.pressure = 0.73f;
    window.deliverTouch(pen);
    check(touches.size() == 2 && touches[0].kind == PointerKind::Pen && touches[0].pressure == 0.0f &&
              touches[1].pressure == 0.73f,
          "a pen hovers without pressure and presses with it");

    // The defaults say "the mouse", so events built by old code are right.
    const PointerEvent plain;
    check(plain.kind == PointerKind::Mouse && plain.id == 0 && plain.primary && plain.delta == Vec2{},
          "a default event is the mouse, with no delta");
    check(!window.isRelativeMouse() && !window.isCursorConfined(), "a window without a backend has neither mode");
    window.setRelativeMouse(true);
    window.setCursorConfined(true);
    check(!window.isRelativeMouse() && !window.isCursorConfined(), "and asking for them does nothing");
}

} // namespace

int main() {
    keysByPosition();
    relativeMotion();
    touchRouting();
    return cfw::test::finish("InputTest");
}
