// Gamepads without hardware: virtual pads driven through the same path real
// ones take (connect and disconnect, press and release and axis events,
// polling, ids, handlers that change the list), and each platform's mapping
// from what the system reports to the standard layout: XInput states, HID
// reports and evdev event sequences.

#include <cmath>
#include <vector>

#include "../src/GamepadMapping.h"
#include "cfw/platform/Gamepad.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

using B = GamepadButton;
using A = GamepadAxis;
using E = GamepadEvent::Type;

void virtualPads() {
    auto pads = Gamepads::create();
    std::vector<GamepadInfo> arrivals;
    std::vector<GamepadId> departures;
    std::vector<GamepadEvent> events;
    ScopedConnection c1 = pads->connected.connect([&](const GamepadInfo &info) { arrivals.push_back(info); });
    ScopedConnection c2 = pads->disconnected.connect([&](GamepadId id) { departures.push_back(id); });
    ScopedConnection c3 = pads->event.connect([&](const GamepadEvent &e) { events.push_back(e); });
    pads->poll();
    // Whatever real pads this machine has are announced by the first poll.
    const std::size_t real = arrivals.size();
    arrivals.clear();
    events.clear();

    const GamepadId id = pads->addVirtual("Test pad");
    check(id != 0, "a pad's id is never 0");
    check(!pads->isConnected(id), "a new pad appears at the next poll, not before");
    pads->poll();
    checkEqual(arrivals.size(), std::size_t(1), "the pad connects");
    check(arrivals[0].id == id && arrivals[0].name == "Test pad" && arrivals[0].standard, "with its id, name and layout");
    check(pads->isConnected(id), "and is connected");
    checkEqual(pads->pads().size(), real + 1, "it is in the list");
    check(!pads->state(id).button(B::A) && pads->state(id).axis(A::LeftX) == 0.0f, "at rest");

    // Press A, push the left stick, half-pull the right trigger.
    GamepadState state;
    state.standard = true;
    detail::setButton(state, B::A, true);
    detail::setAxis(state, A::LeftX, 0.75f);
    detail::setAxis(state, A::RightTrigger, 0.5f);
    pads->setVirtualState(id, state);
    check(!pads->state(id).button(B::A), "state() does not change between polls");
    pads->poll();
    check(pads->state(id).button(B::A) && pads->state(id).axis(A::LeftX) == 0.75f, "polling sees the new state");
    checkEqual(events.size(), std::size_t(3), "one event for each control that changed");
    check(events[0].type == E::ButtonPress && events[0].button == B::A && events[0].id == id && events[0].value == 1.0f,
          "the button press");
    check(events[1].type == E::AxisChange && events[1].axis == A::LeftX && events[1].value == 0.75f, "the stick");
    check(events[2].type == E::AxisChange && events[2].axis == A::RightTrigger && events[2].value == 0.5f, "the trigger");

    events.clear();
    pads->poll();
    check(events.empty(), "nothing changed: no events");
    pads->setVirtualState(id, state);
    pads->poll();
    check(events.empty(), "the same state again: no events");

    detail::setButton(state, B::A, false);
    detail::setButton(state, B::DpadLeft, true);
    detail::setAxis(state, A::LeftX, -1.0f);
    pads->setVirtualState(id, state);
    // Two states between polls: only the last is seen, as with a real pad.
    detail::setButton(state, B::Guide, true);
    pads->setVirtualState(id, state);
    pads->poll();
    checkEqual(events.size(), std::size_t(4), "release, two presses and the stick");
    check(events[0].type == E::ButtonRelease && events[0].button == B::A && events[0].value == 0.0f, "A released");
    check(events[1].type == E::ButtonPress && events[1].button == B::Guide, "Guide pressed");
    check(events[2].type == E::ButtonPress && events[2].button == B::DpadLeft, "d-pad left pressed");
    check(events[3].type == E::AxisChange && events[3].value == -1.0f, "the stick hard left");

    // A handler can ask for the state the event describes, and may change the list.
    bool consistent = true;
    GamepadId second = 0;
    ScopedConnection c4 = pads->event.connect([&](const GamepadEvent &e) {
        if (e.type == E::ButtonPress) {
            consistent = consistent && pads->state(e.id).button(e.button);
            if (!second) {
                second = pads->addVirtual("Joined mid-poll", false);
            }
        }
    });
    detail::setButton(state, B::Start, true);
    pads->setVirtualState(id, state);
    pads->poll();
    check(consistent, "state() inside an event handler already has the event's change");
    check(second != 0 && second != id, "a pad added from a handler gets its own id");
    arrivals.clear();
    pads->poll();
    check(arrivals.size() == 1 && arrivals[0].id == second && !arrivals[0].standard, "and connects at the next poll");

    // A pad that does not map: raw buttons and axes only.
    events.clear();
    GamepadState raw;
    raw.rawButtons = {false, true, false};
    raw.rawAxes = {0.0f, -0.5f};
    detail::setButton(raw, B::A, true); // ignored: the pad is not standard
    pads->setVirtualState(second, raw);
    pads->poll();
    checkEqual(events.size(), std::size_t(2), "raw events only");
    check(events[0].type == E::RawButtonPress && events[0].rawIndex == 1 && events[0].id == second, "raw button 1");
    check(events[1].type == E::RawAxisChange && events[1].rawIndex == 1 && events[1].value == -0.5f, "raw axis 1");
    check(!pads->state(second).standard, "its state says it is not the standard layout");

    check(!pads->rumble(id, 1.0f, 1.0f, std::chrono::milliseconds(100)), "a virtual pad has no motors");
    check(!pads->rumble(9999, 1.0f, 1.0f, std::chrono::milliseconds(100)), "nor does a pad that does not exist");

    // Unplugged.
    pads->removeVirtual(id);
    check(pads->isConnected(id), "a pad goes at the next poll, not before");
    pads->poll();
    check(departures.size() == 1 && departures[0] == id, "the pad disconnects");
    check(!pads->isConnected(id), "and is gone");
    check(!pads->state(id).button(B::Start), "its state is neutral from then on");
    const GamepadId third = pads->addVirtual("Again");
    check(third != id && third != second, "ids are not reused");
    pads->removeVirtual(third);
    arrivals.clear();
    departures.clear();
    pads->poll();
    check(arrivals.empty() && departures.empty(), "a pad that came and went between polls is never announced");
}

void xinputMapping() {
    // A, X, d-pad up, left shoulder and Guide held; left stick up and right;
    // right trigger fully pulled.
    const GamepadState s = detail::stateFromXInput(0x1000 | 0x4000 | 0x0001 | 0x0100 | 0x0400, 0, 255, 32767, 32767, -32768, 0);
    check(s.standard, "an XInput pad is the standard layout");
    check(s.button(B::A) && s.button(B::X) && s.button(B::DpadUp) && s.button(B::LeftShoulder) && s.button(B::Guide),
          "the held buttons");
    check(!s.button(B::B) && !s.button(B::Y) && !s.button(B::Start) && !s.button(B::RightStick), "and no others");
    checkNear(s.axis(A::LeftX), 1.0, 1e-6, "stick right is +1");
    checkNear(s.axis(A::LeftY), -1.0, 1e-6, "stick up is -1: down is positive");
    checkNear(s.axis(A::RightX), -1.0, 1e-6, "the most negative value is exactly -1");
    checkNear(s.axis(A::RightY), 0.0, 1e-6, "centre");
    checkNear(s.axis(A::LeftTrigger), 0.0, 1e-6, "a released trigger is 0");
    checkNear(s.axis(A::RightTrigger), 1.0, 1e-6, "a pulled one is 1");
    checkEqual(s.rawButtons.size(), std::size_t(16), "the raw button bits");
    checkEqual(s.rawAxes.size(), std::size_t(6), "and the six raw axes");
    const GamepadState more = detail::stateFromXInput(0x2000 | 0x8000 | 0x0010 | 0x0020 | 0x0040 | 0x0080 | 0x0200 | 0x000E,
                                                      128, 0, 0, 0, 0, 0);
    check(more.button(B::B) && more.button(B::Y) && more.button(B::Start) && more.button(B::Back) &&
              more.button(B::LeftStick) && more.button(B::RightStick) && more.button(B::RightShoulder) &&
              more.button(B::DpadDown) && more.button(B::DpadLeft) && more.button(B::DpadRight),
          "the rest of the buttons");
    checkNear(more.axis(A::LeftTrigger), 128.0 / 255.0, 1e-6, "a half-pulled trigger");
}

void hidMapping() {
    // A DualShock-style report: Cross and R1 held, Options, hat down-left,
    // left stick left, right stick down, L2 half pulled.
    detail::HidPadReport report;
    report.buttons.assign(14, false);
    report.buttons[1] = true; // Cross
    report.buttons[5] = true; // R1
    report.buttons[9] = true; // Options
    report.hasX = report.hasY = report.hasZ = report.hasRz = report.hasRx = report.hasRy = report.hasHat = true;
    report.x = -1.0f;
    report.rz = 1.0f;
    report.rx = 0.0f;  // L2 half way: the axis runs -1 to 1
    report.ry = -1.0f; // R2 released
    report.hat = 5;    // down-left
    const GamepadState sony = detail::stateFromHid(report, detail::kVendorSony);
    check(sony.standard, "a Sony pad is mapped to the standard layout");
    check(sony.button(B::A) && sony.button(B::RightShoulder) && sony.button(B::Start), "Cross is A, R1, Options is Start");
    check(!sony.button(B::X) && !sony.button(B::B) && !sony.button(B::Y) && !sony.button(B::Back), "nothing else");
    check(sony.button(B::DpadDown) && sony.button(B::DpadLeft) && !sony.button(B::DpadUp) && !sony.button(B::DpadRight),
          "the hat's diagonal is two d-pad buttons");
    checkNear(sony.axis(A::LeftX), -1.0, 1e-6, "left stick");
    checkNear(sony.axis(A::RightY), 1.0, 1e-6, "right stick, on Z and Rz");
    checkNear(sony.axis(A::LeftTrigger), 0.5, 1e-6, "L2 from its axis");
    checkNear(sony.axis(A::RightTrigger), 0.0, 1e-6, "R2 released");
    checkEqual(sony.rawButtons.size(), std::size_t(14), "raw buttons as reported");
    checkEqual(sony.rawAxes.size(), std::size_t(8), "six axes and the hat as two");

    // The same report from an unknown maker: raw only, nothing guessed.
    const GamepadState unknown = detail::stateFromHid(report, 0x1234);
    check(!unknown.standard && !unknown.button(B::A), "an unknown HID pad is not given a layout it may not have");
    check(unknown.rawButtons[1] && unknown.rawAxes.size() == 8, "but all of it is there raw");
    checkNear(unknown.rawAxes[6], -1.0, 1e-6, "the hat as an axis: left");
    checkNear(unknown.rawAxes[7], 1.0, 1e-6, "and down");
    report.hat = 15;
    check(detail::stateFromHid(report, 0x1234).rawAxes[7] == 0.0f, "a centred hat is 0");
}

void evdevMapping() {
    using Pad = detail::EvdevPad;
    // An Xbox pad as the kernel's xpad driver describes it.
    const std::vector<int> keys = {0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x13A, 0x13B, 0x13C, 0x13D, 0x13E};
    const std::vector<Pad::Axis> axes = {{0x00, -32768, 32767, 0}, {0x01, -32768, 32767, 0}, {0x02, 0, 1023, 0},
                                         {0x03, -32768, 32767, 0}, {0x04, -32768, 32767, 0}, {0x05, 0, 1023, 0},
                                         {0x10, -1, 1, 0},         {0x11, -1, 1, 0}};
    Pad xbox;
    xbox.configure(keys, axes, 0x045E);
    check(xbox.isController() && xbox.isGamepad(), "a device with the south button is a gamepad");
    check(!xbox.state().button(B::A) && xbox.state().axis(A::LeftTrigger) == 0.0f, "at rest");
    checkNear(xbox.state().axis(A::LeftX), 0.0, 1e-4, "a centred stick is about 0");
    xbox.handle(Pad::kEventKey, 0x130, 1); // BTN_SOUTH
    xbox.handle(Pad::kEventKey, 0x133, 1); // BTN_NORTH, which the Xbox driver uses for X
    xbox.handle(Pad::kEventAbs, 0x00, 32767);
    xbox.handle(Pad::kEventAbs, 0x01, -32768);
    xbox.handle(Pad::kEventAbs, 0x05, 1023);
    xbox.handle(Pad::kEventAbs, 0x10, -1);
    xbox.handle(Pad::kEventAbs, 0x11, 1);
    xbox.handle(Pad::kEventKey, 0x999, 1);  // a code the device does not have: ignored
    xbox.handle(Pad::kEventAbs, 0x20, 500); // likewise
    GamepadState s = xbox.state();
    check(s.standard && s.button(B::A) && s.button(B::X) && !s.button(B::Y) && !s.button(B::B),
          "south is A; the Xbox driver's \"north\" is X");
    checkNear(s.axis(A::LeftX), 1.0, 1e-6, "stick right");
    checkNear(s.axis(A::LeftY), -1.0, 1e-6, "stick up is -1");
    checkNear(s.axis(A::RightTrigger), 1.0, 1e-6, "the trigger's range is the device's, 0 to 1023");
    check(s.button(B::DpadLeft) && s.button(B::DpadDown) && !s.button(B::DpadRight) && !s.button(B::DpadUp), "the hat");
    checkEqual(s.rawButtons.size(), keys.size(), "raw buttons in the device's order");
    check(s.rawButtons[0] && s.rawButtons[2] && !s.rawButtons[1], "south and north are down");
    checkEqual(s.rawAxes.size(), axes.size(), "raw axes too");
    checkNear(s.rawAxes[5], 1.0, 1e-6, "a raw trigger runs -1 to 1");
    xbox.handle(Pad::kEventKey, 0x130, 0);
    check(!xbox.state().button(B::A), "released");

    // Sony's driver follows the kernel's naming by position: north is
    // Triangle (Y), west is Square (X).
    Pad sony;
    sony.configure(keys, axes, detail::kVendorSony);
    sony.handle(Pad::kEventKey, 0x133, 1);
    check(sony.state().button(B::Y) && !sony.state().button(B::X), "on a Sony pad north is Y");
    sony.handle(Pad::kEventKey, 0x134, 1);
    check(sony.state().button(B::X), "and west is X");

    // A pad with digital triggers and a d-pad of buttons.
    Pad simple;
    simple.configure({0x130, 0x131, 0x138, 0x139, 0x220, 0x221, 0x222, 0x223}, {{0x00, 0, 255, 128}, {0x01, 0, 255, 128}}, 0);
    simple.handle(Pad::kEventKey, 0x138, 1);
    simple.handle(Pad::kEventKey, 0x223, 1);
    s = simple.state();
    check(s.axis(A::LeftTrigger) == 1.0f && s.axis(A::RightTrigger) == 0.0f, "trigger buttons become 0 or 1");
    check(s.button(B::DpadRight) && !s.button(B::DpadLeft), "d-pad buttons");
    checkNear(s.axis(A::LeftX), 0.0, 0.01, "an 8-bit stick at 128 is about centred");

    // A flight stick: a controller, but not a gamepad; raw only.
    Pad stick;
    stick.configure({0x120, 0x121, 0x122}, {{0x00, 0, 1023, 0}, {0x01, 0, 1023, 1023}, {0x06, 0, 255, 255}}, 0x046D);
    stick.handle(Pad::kEventKey, 0x121, 1);
    check(stick.isController() && !stick.isGamepad(), "a joystick is a controller without the standard layout");
    s = stick.state();
    check(!s.standard && s.rawButtons == std::vector<bool>({false, true, false}), "its buttons, raw");
    check(s.rawAxes.size() == 3 && s.rawAxes[0] == -1.0f && s.rawAxes[1] == 1.0f && s.rawAxes[2] == 1.0f, "its axes, raw");

    // A keyboard and a mouse are not controllers.
    Pad keyboard;
    keyboard.configure({0x110, 0x111, 0x1E0}, {}, 0);
    check(!keyboard.isController(), "a device without controller buttons and an axis is ignored");

    checkEqual(detail::normaliseAxis(5, 5, 5), 0.0f, "a degenerate range is 0, not a division by zero");
    checkEqual(detail::normaliseAxis(999, 0, 100), 1.0f, "values past the range clamp");
}

} // namespace

int main() {
    virtualPads();
    xinputMapping();
    hidMapping();
    evdevMapping();
    return cfw::test::finish("GamepadTest");
}
