#include "cfw/ui/Shortcut.h"

#include <array>
#include <cctype>

namespace cfw {

namespace {

struct Named {
    Key key;
    const char *name;
};

// Names beyond letters, digits and F-keys.
constexpr std::array kNamed{
    Named{Key::Tab, "Tab"},           Named{Key::Enter, "Enter"},         Named{Key::Escape, "Esc"},
    Named{Key::Space, "Space"},       Named{Key::Backspace, "Backspace"}, Named{Key::Delete, "Delete"},
    Named{Key::Left, "Left"},         Named{Key::Right, "Right"},         Named{Key::Up, "Up"},
    Named{Key::Down, "Down"},         Named{Key::Home, "Home"},           Named{Key::End, "End"},
    Named{Key::PageUp, "PgUp"},       Named{Key::PageDown, "PgDown"},     Named{Key::Backquote, "`"},
    Named{Key::Minus, "-"},           Named{Key::Equal, "="},             Named{Key::BracketLeft, "["},
    Named{Key::BracketRight, "]"},    Named{Key::Backslash, "\\"},        Named{Key::Semicolon, ";"},
    Named{Key::Quote, "'"},           Named{Key::Comma, ","},             Named{Key::Period, "."},
    Named{Key::Slash, "/"},           Named{Key::Insert, "Ins"},
};

// Other spellings parse() accepts.
constexpr std::array kAliases{
    Named{Key::Enter, "Return"},  Named{Key::Escape, "Escape"},    Named{Key::Delete, "Del"},
    Named{Key::PageUp, "PageUp"}, Named{Key::PageDown, "PageDown"}, Named{Key::Insert, "Insert"},
};

bool equalsIgnoringCase(StringView a, StringView b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

Key offset(Key first, unsigned by) { return static_cast<Key>(static_cast<unsigned>(first) + by); }

std::optional<Key> keyFromName(StringView name) {
    if (name.size() == 1) {
        const char c = name[0];
        if (std::isalpha(static_cast<unsigned char>(c))) {
            return offset(Key::A, unsigned(std::toupper(static_cast<unsigned char>(c)) - 'A'));
        }
        if (c >= '0' && c <= '9') {
            return offset(Key::Digit0, unsigned(c - '0'));
        }
    }
    if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f')) {
        unsigned number = 0;
        bool digits = true;
        for (std::size_t i = 1; i < name.size(); ++i) {
            digits = digits && std::isdigit(static_cast<unsigned char>(name[i]));
            number = number * 10 + unsigned(name[i] - '0');
        }
        if (digits && number >= 1 && number <= 12) {
            return offset(Key::F1, number - 1);
        }
    }
    for (const Named &named : kNamed) {
        if (equalsIgnoringCase(name, named.name)) {
            return named.key;
        }
    }
    for (const Named &named : kAliases) {
        if (equalsIgnoringCase(name, named.name)) {
            return named.key;
        }
    }
    return std::nullopt;
}

} // namespace

StringView keyName(Key key) noexcept {
    static constexpr const char *kLetters[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                               "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    static constexpr const char *kDigits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    static constexpr const char *kFunctions[] = {"F1", "F2", "F3", "F4",  "F5",  "F6",
                                                 "F7", "F8", "F9", "F10", "F11", "F12"};
    const auto k = static_cast<unsigned>(key);
    if (k >= unsigned(Key::A) && k <= unsigned(Key::Z)) {
        return kLetters[k - unsigned(Key::A)];
    }
    if (k >= unsigned(Key::Digit0) && k <= unsigned(Key::Digit9)) {
        return kDigits[k - unsigned(Key::Digit0)];
    }
    if (k >= unsigned(Key::F1) && k <= unsigned(Key::F12)) {
        return kFunctions[k - unsigned(Key::F1)];
    }
    for (const Named &named : kNamed) {
        if (named.key == key) {
            return named.name;
        }
    }
    return {};
}

std::optional<KeyChord> KeyChord::parse(StringView text) {
    KeyChord chord;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t plus = text.find('+', start);
        // A trailing "+" is the key itself only as "Ctrl++"; shortcuts here
        // name it "=" instead, so "+" always separates.
        if (plus == StringView::npos) {
            plus = text.size();
        }
        const StringView part = text.substr(start, plus - start);
        const bool last = plus == text.size();
        if (part.empty()) {
            return std::nullopt;
        }
        if (!last) {
            if (equalsIgnoringCase(part, "Ctrl") || equalsIgnoringCase(part, "Control")) {
                chord.modifiers = chord.modifiers | Modifier::Control;
            } else if (equalsIgnoringCase(part, "Shift")) {
                chord.modifiers = chord.modifiers | Modifier::Shift;
            } else if (equalsIgnoringCase(part, "Alt")) {
                chord.modifiers = chord.modifiers | Modifier::Alt;
            } else if (equalsIgnoringCase(part, "Meta") || equalsIgnoringCase(part, "Cmd")) {
                chord.modifiers = chord.modifiers | Modifier::Meta;
            } else {
                return std::nullopt;
            }
        } else {
            const std::optional<Key> key = keyFromName(part);
            if (!key) {
                return std::nullopt;
            }
            chord.key = *key;
            return chord;
        }
        start = plus + 1;
    }
    return std::nullopt;
}

String KeyChord::text() const {
    String out;
    if (hasModifier(modifiers, Modifier::Control)) {
        out += "Ctrl+";
    }
    if (hasModifier(modifiers, Modifier::Shift)) {
        out += "Shift+";
    }
    if (hasModifier(modifiers, Modifier::Alt)) {
        out += "Alt+";
    }
    if (hasModifier(modifiers, Modifier::Meta)) {
        out += "Meta+";
    }
    out += String(keyName(key));
    return out;
}

bool KeyChord::matches(const KeyEvent &event) const noexcept {
    return event.type == KeyEvent::Type::Press && event.key == key && event.modifiers == modifiers;
}

} // namespace cfw
