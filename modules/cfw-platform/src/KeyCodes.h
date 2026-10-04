#pragma once

// Keys by position. A PC keyboard identifies each key by a scan code that
// does not depend on the layout the user chose: the key right of Caps Lock is
// 0x1E whether it types "a" (QWERTY) or "q" (AZERTY). Windows hands the scan
// code over with every key message; Linux's evdev key codes (an X11 key code
// minus 8) are the same numbers for the main block. This maps them to the
// Key a US QWERTY keyboard has in that position: KeyEvent::physicalKey.
//
// X11 goes one better where the server has the XKB extension (every current
// one): XKB names each key by its position ("AD02" is the second key of the
// row above the home row), which holds even on a server whose key codes are
// not the kernel's.
//
// Pure functions, no OS calls: tested on every platform.

#include "cfw/core/Input.h"
#include "cfw/core/String.h"

namespace cfw::detail {

// The main block, shared by scan code set 1 (Windows) and evdev (Linux).
constexpr Key physicalKeyFromMainBlock(unsigned code) noexcept {
    constexpr Key kRow1[] = {Key::Digit1, Key::Digit2, Key::Digit3, Key::Digit4, Key::Digit5, Key::Digit6,
                             Key::Digit7, Key::Digit8, Key::Digit9, Key::Digit0, Key::Minus,  Key::Equal};
    constexpr Key kRow2[] = {Key::Q, Key::W, Key::E, Key::R, Key::T, Key::Y, Key::U, Key::I, Key::O, Key::P,
                             Key::BracketLeft, Key::BracketRight};
    constexpr Key kRow3[] = {Key::A, Key::S, Key::D, Key::F, Key::G, Key::H, Key::J, Key::K, Key::L,
                             Key::Semicolon, Key::Quote, Key::Backquote};
    constexpr Key kRow4[] = {Key::Z, Key::X, Key::C, Key::V, Key::B, Key::N, Key::M, Key::Comma, Key::Period, Key::Slash};
    if (code >= 0x02 && code <= 0x0D) {
        return kRow1[code - 0x02];
    }
    if (code >= 0x10 && code <= 0x1B) {
        return kRow2[code - 0x10];
    }
    if (code >= 0x1E && code <= 0x29) {
        return kRow3[code - 0x1E];
    }
    if (code >= 0x2C && code <= 0x35) {
        return kRow4[code - 0x2C];
    }
    if (code >= 0x3B && code <= 0x44) {
        return static_cast<Key>(static_cast<unsigned>(Key::F1) + (code - 0x3B));
    }
    switch (code) {
    case 0x01: return Key::Escape;
    case 0x0E: return Key::Backspace;
    case 0x0F: return Key::Tab;
    case 0x1C: return Key::Enter;
    case 0x1D: return Key::LeftControl;
    case 0x2A: return Key::LeftShift;
    case 0x2B: return Key::Backslash;
    case 0x36: return Key::RightShift;
    case 0x38: return Key::LeftAlt;
    case 0x39: return Key::Space;
    case 0x57: return Key::F11;
    case 0x58: return Key::F12;
    default: return Key::Unknown;
    }
}

// A Windows key message's scan code (bits 16-23 of lParam) and its
// "extended" flag (bit 24), which tells the right-hand Control and Alt and
// the navigation block from the keys that share their codes.
constexpr Key physicalKeyFromScanCode(unsigned scanCode, bool extended) noexcept {
    if (!extended) {
        return physicalKeyFromMainBlock(scanCode);
    }
    switch (scanCode) {
    case 0x1C: return Key::Enter; // the number pad's
    case 0x1D: return Key::RightControl;
    case 0x35: return Key::Slash; // the number pad's
    case 0x38: return Key::RightAlt;
    case 0x47: return Key::Home;
    case 0x48: return Key::Up;
    case 0x49: return Key::PageUp;
    case 0x4B: return Key::Left;
    case 0x4D: return Key::Right;
    case 0x4F: return Key::End;
    case 0x50: return Key::Down;
    case 0x51: return Key::PageDown;
    case 0x52: return Key::Insert;
    case 0x53: return Key::Delete;
    case 0x5B: return Key::LeftMeta;
    case 0x5C: return Key::RightMeta;
    default: return Key::Unknown;
    }
}

// A Linux evdev key code (KEY_* in linux/input-event-codes.h). An X11 key
// code is this plus 8 on every server that takes its keys from evdev or
// libinput, which is every current one.
constexpr Key physicalKeyFromEvdev(unsigned code) noexcept {
    switch (code) {
    case 96: return Key::Enter; // KEY_KPENTER
    case 97: return Key::RightControl;
    case 98: return Key::Slash; // KEY_KPSLASH
    case 100: return Key::RightAlt;
    case 102: return Key::Home;
    case 103: return Key::Up;
    case 104: return Key::PageUp;
    case 105: return Key::Left;
    case 106: return Key::Right;
    case 107: return Key::End;
    case 108: return Key::Down;
    case 109: return Key::PageDown;
    case 110: return Key::Insert;
    case 111: return Key::Delete;
    case 125: return Key::LeftMeta;
    case 126: return Key::RightMeta;
    default: return code < 0x60 ? physicalKeyFromMainBlock(code) : Key::Unknown;
    }
}

// An XKB key name (up to four characters, as in xkb/keycodes): rows AE (the
// digits), AD, AC and AB from top to bottom with the column number, and
// mnemonics for the rest.
constexpr Key physicalKeyFromXkbName(StringView name) noexcept {
    constexpr Key kRowE[] = {Key::Digit1, Key::Digit2, Key::Digit3, Key::Digit4, Key::Digit5, Key::Digit6,
                             Key::Digit7, Key::Digit8, Key::Digit9, Key::Digit0, Key::Minus,  Key::Equal};
    constexpr Key kRowD[] = {Key::Q, Key::W, Key::E, Key::R, Key::T, Key::Y, Key::U, Key::I, Key::O, Key::P,
                             Key::BracketLeft, Key::BracketRight};
    constexpr Key kRowC[] = {Key::A, Key::S, Key::D, Key::F, Key::G, Key::H, Key::J, Key::K, Key::L,
                             Key::Semicolon, Key::Quote};
    constexpr Key kRowB[] = {Key::Z, Key::X, Key::C, Key::V, Key::B, Key::N, Key::M, Key::Comma, Key::Period, Key::Slash};
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (name.size() == 4 && digit(name[2]) && digit(name[3])) {
        const unsigned column = unsigned(name[2] - '0') * 10 + unsigned(name[3] - '0');
        if (name[0] == 'A' && column >= 1) {
            switch (name[1]) {
            case 'E': return column <= 12 ? kRowE[column - 1] : Key::Unknown;
            case 'D': return column <= 12 ? kRowD[column - 1] : Key::Unknown;
            case 'C': return column <= 11 ? kRowC[column - 1] : Key::Unknown;
            case 'B': return column <= 10 ? kRowB[column - 1] : Key::Unknown;
            default: return Key::Unknown;
            }
        }
        if (name[0] == 'F' && name[1] == 'K' && column >= 1 && column <= 12) {
            return static_cast<Key>(static_cast<unsigned>(Key::F1) + column - 1);
        }
        return Key::Unknown;
    }
    constexpr struct {
        StringView name;
        Key key;
    } kNamed[] = {
        {"TLDE", Key::Backquote}, {"BKSL", Key::Backslash}, {"SPCE", Key::Space},       {"TAB", Key::Tab},
        {"RTRN", Key::Enter},     {"KPEN", Key::Enter},     {"ESC", Key::Escape},       {"BKSP", Key::Backspace},
        {"DELE", Key::Delete},    {"INS", Key::Insert},     {"HOME", Key::Home},        {"END", Key::End},
        {"PGUP", Key::PageUp},    {"PGDN", Key::PageDown},  {"UP", Key::Up},            {"DOWN", Key::Down},
        {"LEFT", Key::Left},      {"RGHT", Key::Right},     {"LFSH", Key::LeftShift},   {"RTSH", Key::RightShift},
        {"LCTL", Key::LeftControl}, {"RCTL", Key::RightControl}, {"LALT", Key::LeftAlt}, {"RALT", Key::RightAlt},
        {"LWIN", Key::LeftMeta},  {"RWIN", Key::RightMeta}, {"LMTA", Key::LeftMeta},    {"RMTA", Key::RightMeta},
    };
    for (const auto &entry : kNamed) {
        if (entry.name == name) {
            return entry.key;
        }
    }
    return Key::Unknown;
}

} // namespace cfw::detail
