#pragma once

// Input events: produced by cfw-platform windows (or tests, or an embedding
// such as the Qt editor during the port) and consumed by cfw-ui. Plain data,
// no OS calls. Positions are logical pixels (physical / device pixel ratio).
//
// Threads: plain value types.

#include <cstdint>

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

struct PointerEvent {
    enum class Type : std::uint8_t { Move, Press, Release, Wheel, Leave };
    Type type = Type::Move;
    Vec2 position;
    PointerButton button = PointerButton::None;
    Modifier modifiers = Modifier::None;
    Vec2 wheelDelta; // pixels; positive y scrolls content up
    int clickCount = 1; // 2 for a double-click press
};

// Physical, layout-independent keys the toolkit itself cares about. Text
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
    Shift, Control, Alt, Meta, // the modifier keys themselves
};

struct KeyEvent {
    enum class Type : std::uint8_t { Press, Release };
    Type type = Type::Press;
    Key key = Key::Unknown;
    Modifier modifiers = Modifier::None;
    bool repeat = false;
};

// The pointer's shape over an element or a window.
enum class Cursor : std::uint8_t {
    Arrow, IBeam, Hand, Wait, Crosshair, SizeHorizontal, SizeVertical, SizeDiagonal, SizeAntiDiagonal, SizeAll,
    NotAllowed, Hidden,
};

struct TextEvent {
    String text; // UTF-8, already composed (IME commits arrive here too)
};

} // namespace cfw
