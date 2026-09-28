#pragma once

// Input events as cfw-ui sees them, independent of where they came from
// (cfw-platform windows, tests, or an embedding such as the Qt editor during
// the port). Positions are in the surface's logical pixels.
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
    A, C, V, X, Y, Z, // for shortcuts
    F1, F2, F5,
};

struct KeyEvent {
    enum class Type : std::uint8_t { Press, Release };
    Type type = Type::Press;
    Key key = Key::Unknown;
    Modifier modifiers = Modifier::None;
    bool repeat = false;
};

struct TextEvent {
    String text; // UTF-8, already composed (IME commits arrive here too)
};

} // namespace cfw
