#pragma once

// Input events: produced by cfw-platform windows (or tests, or an embedding
// such as the Qt editor during the port) and consumed by cfw-ui. Plain data,
// no OS calls. Positions are logical pixels (physical / device pixel ratio).
//
// Threads: plain value types.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cfw/core/String.h"
#include "cfw/core/Vec2.h"

namespace cfw {

enum class Modifier : std::uint8_t { None = 0, Shift = 1, Control = 2, Alt = 4, Meta = 8 };
constexpr Modifier operator|(Modifier a, Modifier b) noexcept {
    return static_cast<Modifier>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr bool hasModifier(Modifier set, Modifier m) noexcept {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(m)) != 0;
}

enum class PointerButton : std::uint8_t { None, Left, Right, Middle };

// What is pointing: the mouse (or a trackpad acting as one), a finger on a
// touch screen, or a pen.
enum class PointerKind : std::uint8_t { Mouse, Touch, Pen };

// A window reports pointers on two signals (see Window.h):
//
//   Window::pointer  the mouse, and the primary touch or pen acting as one:
//                    its Press, Move and Release arrive exactly as a left
//                    mouse button's do, so code written for the mouse works
//                    with a finger without knowing it. One pointer at a time.
//   Window::touch    every finger and pen, the primary one included, each
//                    with its own id, so several can be followed at once.
struct PointerEvent {
    // Cancel: the system took a touch or pen contact away (a palm, a system
    // gesture). It ends the contact as Release does, but must not act as a
    // click or a tap. The mouse never produces it.
    enum class Type : std::uint8_t { Move, Press, Release, Wheel, Leave, Cancel };
    Type type = Type::Move;
    Vec2 position;
    PointerButton button = PointerButton::None;
    Modifier modifiers = Modifier::None;
    Vec2 wheelDelta; // pixels; positive y scrolls content up
    int clickCount = 1; // 2 for a double-click press
    // Relative mouse mode only (Window::setRelativeMouse): how far the mouse
    // itself moved since the last event, in the device's own counts, right
    // and down positive, before the system's pointer acceleration and
    // unaffected by the screen's edges. Zero otherwise. `position` does not
    // move while the mode is on.
    Vec2 delta;
    PointerKind kind = PointerKind::Mouse;
    // Which contact, for touch and pen: the same from its Press to its
    // Release or Cancel, and different from every other contact that is down
    // at the same time. 0 for the mouse.
    std::uint32_t id = 0;
    // The mouse; or the first finger (or the pen) of a touch sequence, the
    // one that also acts as the mouse on Window::pointer.
    bool primary = true;
    // 0 to 1 where the device measures it (pens, some touch screens); 1 for
    // anything pressed that does not, 0 for a hovering pen.
    float pressure = 1.0f;
};

// Keys. A KeyEvent carries a key twice (see KeyEvent): the key the user's
// layout says it is, and the key by its position on the keyboard. Text
// arrives separately as TextEvent.
enum class Key : std::uint16_t {
    Unknown,
    Tab, Enter, Escape, Space, Backspace, Delete,
    Left, Right, Up, Down, Home, End, PageUp, PageDown,
    // Letters, digits and function keys are contiguous, so a backend can map
    // a range by offset.
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    // Punctuation, by position on a US layout (what shortcuts name).
    Backquote, Minus, Equal, BracketLeft, BracketRight, Backslash, Semicolon, Quote, Comma, Period, Slash, Insert,
    Shift, Control, Alt, Meta, // the modifier keys themselves: either one of the pair
    // The same, by side. A window reports these in KeyEvent::physicalKey;
    // KeyEvent::key stays Shift, Control, Alt or Meta, so code that asks "is
    // this Shift?" keeps working.
    LeftShift, RightShift, LeftControl, RightControl, LeftAlt, RightAlt, LeftMeta, RightMeta,
};

// Shift for LeftShift and RightShift, and so on; any other key is itself.
[[nodiscard]] constexpr Key eitherSide(Key key) noexcept {
    switch (key) {
    case Key::LeftShift: case Key::RightShift: return Key::Shift;
    case Key::LeftControl: case Key::RightControl: return Key::Control;
    case Key::LeftAlt: case Key::RightAlt: return Key::Alt;
    case Key::LeftMeta: case Key::RightMeta: return Key::Meta;
    default: return key;
    }
}
[[nodiscard]] constexpr bool isModifierKey(Key key) noexcept {
    const Key either = eitherSide(key);
    return either == Key::Shift || either == Key::Control || either == Key::Alt || either == Key::Meta;
}

struct KeyEvent {
    enum class Type : std::uint8_t { Press, Release };
    Type type = Type::Press;
    // The key the user's keyboard layout produces: Key::A is the key that
    // types "a", wherever it is (left of S on a QWERTY keyboard, left of Z on
    // AZERTY). What shortcuts mean: Ctrl+Z is the Z the user sees. Modifier
    // keys are Shift, Control, Alt or Meta whichever side was pressed.
    Key key = Key::Unknown;
    Modifier modifiers = Modifier::None;
    bool repeat = false;
    // The key by its position, named for what a US QWERTY keyboard has
    // there: Key::W is the key above S on every layout. What movement keys
    // mean: W, A, S, D stay in one cluster on AZERTY and Dvorak. Modifier
    // keys are LeftShift, RightShift and so on. Unknown where the platform
    // does not say, and in events built by code that only sets `key`.
    Key physicalKey = Key::Unknown;
};

// The pointer's shape over an element or a window.
enum class Cursor : std::uint8_t {
    Arrow, IBeam, Hand, Wait, Crosshair, SizeHorizontal, SizeVertical, SizeDiagonal, SizeAntiDiagonal, SizeAll,
    NotAllowed, Hidden,
};

struct TextEvent {
    String text; // UTF-8, already composed (IME commits arrive here too)
};

// Text an input method is composing (pinyin being converted, a Hangul
// syllable being built, a dead key's accent): shown at the caret,
// underlined, until the input method commits it (a TextEvent with the
// result) or cancels it. Empty text ends the composition.
struct CompositionEvent {
    String text;            // UTF-8
    std::size_t cursor = 0; // the input method's cursor, a byte offset in text
};

// Files dragged over a window from another application (a file manager).
// Enter comes first, Moves follow as the pointer moves, and it ends with
// either Leave or Drop.
struct DropEvent {
    enum class Type : std::uint8_t { Enter, Move, Leave, Drop };
    Type type = Type::Move;
    Vec2 position;
    // UTF-8 file paths. Always on Drop; on Enter and Move only where the
    // platform offers them before the drop (Windows), else empty.
    std::vector<String> paths;
};

// ---- Gamepads (produced by cfw-platform's Gamepads) ---------------------------

// The standard layout: what an Xbox pad has, and what a PlayStation or
// Switch pad has under other names. Face buttons are named by position as on
// an Xbox pad: A is the bottom one (Cross on PlayStation, B on Switch), B the
// right, X the left, Y the top. Values never change: append.
enum class GamepadButton : std::uint8_t {
    A, B, X, Y,
    LeftShoulder, RightShoulder,
    Back,  // View, Share, Select, Minus
    Start, // Menu, Options, Plus
    Guide, // Xbox, PS, Home
    LeftStick, RightStick, // pressing a stick in
    DpadUp, DpadDown, DpadLeft, DpadRight,
};
inline constexpr std::size_t kGamepadButtonCount = 15;

enum class GamepadAxis : std::uint8_t {
    LeftX, LeftY,   // -1 to 1; right and down are positive
    RightX, RightY, // the same
    LeftTrigger, RightTrigger, // 0 (released) to 1
};
inline constexpr std::size_t kGamepadAxisCount = 6;

// Identifies a connected pad. Never 0, and not reused while the program runs:
// a pad unplugged and plugged back in gets a new id.
using GamepadId = std::uint32_t;

// What a pad's controls are doing. Values are as the device reports them,
// normalised and nothing else: no dead zone is applied, so a stick at rest
// reads a little off zero, and deciding how much to ignore is the caller's.
struct GamepadState {
    // The standard layout. Meaningful only when `standard` is true: the pad
    // is one whose buttons and axes are known to be where the layout says.
    bool standard = false;
    std::array<bool, kGamepadButtonCount> buttons{};
    std::array<float, kGamepadAxisCount> axes{};
    // Everything the device has, in the device's own order, for pads that do
    // not map (flight sticks, wheels, unknown pads) and for letting a user
    // bind controls by hand. Axes are -1 to 1; a hat switch is two of them.
    std::vector<bool> rawButtons;
    std::vector<float> rawAxes;

    [[nodiscard]] bool button(GamepadButton b) const noexcept { return buttons[static_cast<std::size_t>(b)]; }
    [[nodiscard]] float axis(GamepadAxis a) const noexcept { return axes[static_cast<std::size_t>(a)]; }
};

struct GamepadInfo {
    GamepadId id = 0;
    String name;                // as the system or the device gives it
    bool standard = false;      // has the standard layout (see GamepadState)
    bool canRumble = false;
    std::uint16_t vendorId = 0; // USB ids; 0 where unknown
    std::uint16_t productId = 0;
};

struct GamepadEvent {
    enum class Type : std::uint8_t {
        ButtonPress, ButtonRelease, AxisChange,          // the standard layout: `button` or `axis`
        RawButtonPress, RawButtonRelease, RawAxisChange, // the device's own: `rawIndex`
    };
    Type type = Type::ButtonPress;
    GamepadId id = 0;
    GamepadButton button = GamepadButton::A;
    GamepadAxis axis = GamepadAxis::LeftX;
    int rawIndex = 0;
    float value = 0.0f; // the axis's new value; 1 or 0 for a button
};

} // namespace cfw
