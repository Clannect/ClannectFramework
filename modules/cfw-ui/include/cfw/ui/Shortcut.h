#pragma once

// Keyboard shortcuts: a key with modifiers, written the way menus show them
// ("Ctrl+Shift+Z", "Delete", "F5").

#include <optional>

#include "cfw/core/Input.h"
#include "cfw/core/String.h"

namespace cfw {

struct KeyChord {
    Key key = Key::Unknown;
    Modifier modifiers = Modifier::None;

    // "Ctrl+S", "Shift+F5", "Ctrl+Shift+Z", "Delete", "Space", "1". Modifier
    // names are Ctrl (or Control), Shift, Alt and Meta (or Cmd), in any case
    // and order; nothing when the text names no known key.
    [[nodiscard]] static std::optional<KeyChord> parse(StringView text);
    // The text a menu shows: modifiers in the order Ctrl, Shift, Alt, Meta.
    [[nodiscard]] String text() const;
    // Whether a key press is this chord (modifiers must match exactly).
    [[nodiscard]] bool matches(const KeyEvent &event) const noexcept;

    friend bool operator==(const KeyChord &, const KeyChord &) = default;
};

// The name of a key as shortcuts write it ("A", "F5", "Delete"), or empty.
[[nodiscard]] StringView keyName(Key key) noexcept;

} // namespace cfw
