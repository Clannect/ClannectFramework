// macOS gamepads: the GameController framework. Every pad with the
// "extended gamepad" profile (Xbox, PlayStation and MFi pads, which the
// system maps itself) is reported in the standard layout; its values are
// read in poll(). Rumble needs CoreHaptics and is not implemented.
//
// UNVERIFIED: written without a Mac, like the rest of the macOS backend
// (docs/decisions/0016). No compiler has checked it until the macos workflow
// runs, and no pad has been read through it.

#import <GameController/GameController.h>

#include <vector>

#include "GamepadMapping.h"
#include "cfw/platform/Gamepad.h"

namespace cfw {

namespace {

String fromNs(NSString *text) {
    const char *utf8 = text ? [text UTF8String] : nullptr;
    return utf8 ? String(utf8) : String();
}

class GamepadsCocoa final : public Gamepads {
public:
    GamepadsCocoa() {
        // Pads in the background too: a game window is not always key.
        if (@available(macOS 11.3, *)) {
            GCController.shouldMonitorBackgroundEvents = YES;
        }
    }

protected:
    void readDevices() override {
        @autoreleasepool {
            NSArray<GCController *> *controllers = [GCController controllers];
            // Gone: known pads that are no longer in the system's list.
            for (auto it = m_pads.begin(); it != m_pads.end();) {
                if (![controllers containsObject:it->controller]) {
                    deviceRemoved(it->id);
                    it = m_pads.erase(it);
                } else {
                    ++it;
                }
            }
            for (GCController *controller in controllers) {
                GCExtendedGamepad *pad = [controller extendedGamepad];
                if (!pad) {
                    continue; // a remote or a micro gamepad
                }
                Known *known = nullptr;
                for (Known &entry : m_pads) {
                    if (entry.controller == controller) {
                        known = &entry;
                    }
                }
                if (!known) {
                    GamepadInfo info;
                    info.name = fromNs([controller vendorName]);
                    if (info.name.empty()) {
                        info.name = "Gamepad";
                    }
                    info.standard = true;
                    m_pads.push_back({controller, deviceAdded(std::move(info))});
                    known = &m_pads.back();
                }
                GamepadState state;
                state.standard = true;
                using B = GamepadButton;
                const auto set = [&state](B button, GCControllerButtonInput *input) {
                    detail::setButton(state, button, input && [input isPressed]);
                    state.rawButtons.push_back(input && [input isPressed]);
                };
                set(B::A, [pad buttonA]);
                set(B::B, [pad buttonB]);
                set(B::X, [pad buttonX]);
                set(B::Y, [pad buttonY]);
                set(B::LeftShoulder, [pad leftShoulder]);
                set(B::RightShoulder, [pad rightShoulder]);
                set(B::Back, [pad buttonOptions]);
                set(B::Start, [pad buttonMenu]);
                set(B::Guide, [pad buttonHome]);
                set(B::LeftStick, [pad leftThumbstickButton]);
                set(B::RightStick, [pad rightThumbstickButton]);
                set(B::DpadUp, [[pad dpad] up]);
                set(B::DpadDown, [[pad dpad] down]);
                set(B::DpadLeft, [[pad dpad] left]);
                set(B::DpadRight, [[pad dpad] right]);
                // The framework's sticks are up-positive; the standard layout is down-positive.
                const float axes[6] = {[[[pad leftThumbstick] xAxis] value],  -[[[pad leftThumbstick] yAxis] value],
                                       [[[pad rightThumbstick] xAxis] value], -[[[pad rightThumbstick] yAxis] value],
                                       [[pad leftTrigger] value],             [[pad rightTrigger] value]};
                for (std::size_t i = 0; i < 6; ++i) {
                    state.axes[i] = axes[i];
                    state.rawAxes.push_back(i < 4 ? axes[i] : axes[i] * 2.0f - 1.0f);
                }
                deviceState(known->id, std::move(state));
            }
        }
    }

private:
    struct Known {
        GCController *controller;
        GamepadId id;
    };
    std::vector<Known> m_pads;
};

} // namespace

std::unique_ptr<Gamepads> Gamepads::create() { return std::make_unique<GamepadsCocoa>(); }

} // namespace cfw
