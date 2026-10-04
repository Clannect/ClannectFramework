// Prints every input event a window and the gamepads produce, for checking
// by hand with a real keyboard, mouse, touch screen, pen and pad.
//
//     cfw-input-events
//
//   R        toggles relative mouse mode (it also ends when the window loses
//            the focus: Alt+Tab away and back to see it stay off)
//   C        toggles confining the cursor to the window
//   V        rumbles every pad that can, for half a second
//   Escape   leaves relative mode, or quits
//
// Mouse moves are printed once per frame (the last one), so the output stays
// readable; everything else is printed as it arrives.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cfw/image/Image.h"
#include "cfw/platform/Gamepad.h"
#include "cfw/platform/Window.h"

using namespace cfw;

namespace {

const char *name(PointerKind kind) {
    switch (kind) {
    case PointerKind::Mouse: return "mouse";
    case PointerKind::Touch: return "touch";
    case PointerKind::Pen: return "pen";
    }
    return "?";
}

const char *name(PointerEvent::Type type) {
    switch (type) {
    case PointerEvent::Type::Move: return "move";
    case PointerEvent::Type::Press: return "press";
    case PointerEvent::Type::Release: return "release";
    case PointerEvent::Type::Wheel: return "wheel";
    case PointerEvent::Type::Leave: return "leave";
    case PointerEvent::Type::Cancel: return "cancel";
    }
    return "?";
}

const char *name(PointerButton button) {
    switch (button) {
    case PointerButton::Left: return "left";
    case PointerButton::Right: return "right";
    case PointerButton::Middle: return "middle";
    case PointerButton::None: break;
    }
    return "-";
}

std::string name(Key key) {
    const auto k = static_cast<unsigned>(key);
    const auto in = [k](Key first, Key last) { return k >= unsigned(first) && k <= unsigned(last); };
    if (in(Key::A, Key::Z)) {
        return std::string(1, char('A' + (k - unsigned(Key::A))));
    }
    if (in(Key::Digit0, Key::Digit9)) {
        return std::string(1, char('0' + (k - unsigned(Key::Digit0))));
    }
    if (in(Key::F1, Key::F12)) {
        return "F" + std::to_string(k - unsigned(Key::F1) + 1);
    }
    static const char *const kNames[] = {"Unknown", "Tab", "Enter", "Escape", "Space", "Backspace", "Delete", "Left",
                                         "Right", "Up", "Down", "Home", "End", "PageUp", "PageDown"};
    if (k < sizeof kNames / sizeof *kNames) {
        return kNames[k];
    }
    static const char *const kLater[] = {"Backquote", "Minus", "Equal", "BracketLeft", "BracketRight", "Backslash",
                                         "Semicolon", "Quote", "Comma", "Period", "Slash", "Insert", "Shift", "Control",
                                         "Alt", "Meta", "LeftShift", "RightShift", "LeftControl", "RightControl", "LeftAlt",
                                         "RightAlt", "LeftMeta", "RightMeta"};
    const unsigned later = k - unsigned(Key::Backquote);
    return k >= unsigned(Key::Backquote) && later < sizeof kLater / sizeof *kLater ? kLater[later] : "?";
}

std::string name(Modifier modifiers) {
    std::string text;
    if (hasModifier(modifiers, Modifier::Shift)) text += "Shift ";
    if (hasModifier(modifiers, Modifier::Control)) text += "Control ";
    if (hasModifier(modifiers, Modifier::Alt)) text += "Alt ";
    if (hasModifier(modifiers, Modifier::Meta)) text += "Meta ";
    return text.empty() ? "-" : text.substr(0, text.size() - 1);
}

const char *name(GamepadButton button) {
    static const char *const kNames[kGamepadButtonCount] = {"A", "B", "X", "Y", "LeftShoulder", "RightShoulder", "Back",
                                                            "Start", "Guide", "LeftStick", "RightStick", "DpadUp",
                                                            "DpadDown", "DpadLeft", "DpadRight"};
    return kNames[static_cast<std::size_t>(button)];
}

const char *name(GamepadAxis axis) {
    static const char *const kNames[kGamepadAxisCount] = {"LeftX", "LeftY", "RightX", "RightY", "LeftTrigger",
                                                          "RightTrigger"};
    return kNames[static_cast<std::size_t>(axis)];
}

void print(const char *signal, const PointerEvent &e) {
    std::printf("%-7s %-5s %-7s at %7.1f,%7.1f", signal, name(e.kind), name(e.type), double(e.position.x),
                double(e.position.y));
    if (e.type == PointerEvent::Type::Press || e.type == PointerEvent::Type::Release) {
        std::printf("  button %s  clicks %d", name(e.button), e.clickCount);
    }
    if (e.type == PointerEvent::Type::Wheel) {
        std::printf("  wheel %.1f,%.1f", double(e.wheelDelta.x), double(e.wheelDelta.y));
    }
    if (e.delta.x != 0.0f || e.delta.y != 0.0f) {
        std::printf("  delta %+.0f,%+.0f", double(e.delta.x), double(e.delta.y));
    }
    if (e.kind != PointerKind::Mouse) {
        std::printf("  id %u%s  pressure %.2f", e.id, e.primary ? " primary" : "", double(e.pressure));
    }
    std::printf("  mods %s\n", name(e.modifiers).c_str());
}

} // namespace

int main() {
    auto created = Window::create({"CFW input events: R relative, C confine, V rumble, Esc quit", {640, 400}});
    if (!created) {
        std::fprintf(stderr, "no window: %s\n", created.error().message().c_str());
        return 1;
    }
    std::unique_ptr<Window> window = std::move(created).value();
    std::unique_ptr<Gamepads> pads = Gamepads::create();
    bool open = true;
    std::optional<PointerEvent> lastMove; // printed once a frame
    Vec2 frameDelta;

    ScopedConnection onPointer = window->pointer.connect([&](const PointerEvent &e) {
        if (e.type == PointerEvent::Type::Move && e.kind == PointerKind::Mouse) {
            lastMove = e;
            frameDelta = frameDelta + e.delta;
            return;
        }
        print("pointer", e);
    });
    ScopedConnection onTouch = window->touch.connect([&](const PointerEvent &e) { print("touch", e); });
    ScopedConnection onKey = window->key.connect([&](const KeyEvent &e) {
        std::printf("key     %-7s key %-12s physical %-12s mods %s%s\n", e.type == KeyEvent::Type::Press ? "press" : "release",
                    name(e.key).c_str(), name(e.physicalKey).c_str(), name(e.modifiers).c_str(), e.repeat ? "  (repeat)" : "");
        if (e.type != KeyEvent::Type::Press || e.repeat) {
            return;
        }
        // By position, so the keys are where the title says on any layout.
        if (e.physicalKey == Key::R) {
            window->setRelativeMouse(!window->isRelativeMouse());
        } else if (e.physicalKey == Key::C) {
            window->setCursorConfined(!window->isCursorConfined());
            std::printf("        cursor confined: %s\n", window->isCursorConfined() ? "yes" : "no");
        } else if (e.physicalKey == Key::V) {
            for (const GamepadInfo &pad : pads->pads()) {
                const bool ok = pads->rumble(pad.id, 0.6f, 0.3f, std::chrono::milliseconds(500));
                std::printf("        rumble pad %u: %s\n", pad.id, ok ? "started" : "not available");
            }
        } else if (e.key == Key::Escape) {
            if (window->isRelativeMouse()) {
                window->setRelativeMouse(false);
            } else {
                open = false;
            }
        }
    });
    ScopedConnection onText = window->text.connect([](const TextEvent &e) { std::printf("text    \"%s\"\n", e.text.c_str()); });
    ScopedConnection onRelative = window->relativeMouseChanged.connect(
        [](bool on) { std::printf("relative mouse mode: %s\n", on ? "ON" : "off"); });
    ScopedConnection onFocus = window->focusChanged.connect([](bool focused) { std::printf("focus   %s\n", focused ? "gained" : "lost"); });
    ScopedConnection onClose = window->closeRequested.connect([&] { open = false; });

    ScopedConnection onPad = pads->connected.connect([](const GamepadInfo &info) {
        std::printf("gamepad %u connected: \"%s\"%s%s  %04x:%04x\n", info.id, info.name.c_str(),
                    info.standard ? "  standard layout" : "  raw only", info.canRumble ? "  rumble" : "", info.vendorId,
                    info.productId);
    });
    ScopedConnection onPadGone = pads->disconnected.connect([](GamepadId id) { std::printf("gamepad %u disconnected\n", id); });
    ScopedConnection onPadEvent = pads->event.connect([](const GamepadEvent &e) {
        switch (e.type) {
        case GamepadEvent::Type::ButtonPress:
        case GamepadEvent::Type::ButtonRelease:
            std::printf("gamepad %u %-7s %s\n", e.id, e.type == GamepadEvent::Type::ButtonPress ? "press" : "release", name(e.button));
            break;
        case GamepadEvent::Type::AxisChange:
            std::printf("gamepad %u axis    %-12s %+.3f\n", e.id, name(e.axis), double(e.value));
            break;
        case GamepadEvent::Type::RawButtonPress:
        case GamepadEvent::Type::RawButtonRelease:
            std::printf("gamepad %u raw button %d %s\n", e.id, e.rawIndex,
                        e.type == GamepadEvent::Type::RawButtonPress ? "press" : "release");
            break;
        case GamepadEvent::Type::RawAxisChange:
            std::printf("gamepad %u raw axis %d %+.3f\n", e.id, e.rawIndex, double(e.value));
            break;
        }
    });

    // Something to look at: a dark frame, repainted when the system asks.
    bool repaint = true;
    ScopedConnection onRepaint = window->repaintRequested.connect([&] { repaint = true; });
    ScopedConnection onResize = window->resized.connect([&](Vec2i) { repaint = true; });
    while (open) {
        processEvents(std::chrono::milliseconds(8));
        pads->poll();
        if (lastMove) {
            if (frameDelta.x != 0.0f || frameDelta.y != 0.0f) {
                lastMove->delta = frameDelta;
            }
            print("pointer", *lastMove);
            lastMove.reset();
            frameDelta = {};
        }
        if (repaint) {
            repaint = false;
            const Vec2i size = window->pixelSize();
            auto frame = Image::create(std::uint32_t(std::max(1, size.x)), std::uint32_t(std::max(1, size.y)));
            if (frame) {
                for (std::uint8_t &byte : frame.value().pixels()) {
                    byte = 40;
                }
                window->present(frame.value());
            }
        }
        std::fflush(stdout);
    }
    return 0;
}
