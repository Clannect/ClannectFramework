#pragma once

// From what each system reports about a pad to GamepadState. Pure functions
// and plain data, with no OS calls, so every platform's mapping is tested on
// every platform.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "cfw/core/Input.h"

namespace cfw::detail {

inline void setButton(GamepadState &state, GamepadButton button, bool down) noexcept {
    state.buttons[static_cast<std::size_t>(button)] = down;
}
inline void setAxis(GamepadState &state, GamepadAxis axis, float value) noexcept {
    state.axes[static_cast<std::size_t>(axis)] = value;
}

// A value in [minimum, maximum] to -1..1 (or 0..1): the middle of the range
// is 0, so a stick's rest position is, give or take the device's own error.
inline float normaliseAxis(std::int64_t value, std::int64_t minimum, std::int64_t maximum, bool oneSided = false) noexcept {
    if (maximum <= minimum) {
        return 0.0f;
    }
    const double unit = double(std::clamp(value, minimum, maximum) - minimum) / double(maximum - minimum);
    return float(oneSided ? unit : unit * 2.0 - 1.0);
}

// ---- Windows: XInput ----------------------------------------------------------

// XINPUT_GAMEPAD's fields. `buttons` may carry the Guide button (0x0400),
// which only XInputGetStateEx reports.
inline GamepadState stateFromXInput(std::uint16_t buttons, std::uint8_t leftTrigger, std::uint8_t rightTrigger,
                                    std::int16_t leftX, std::int16_t leftY, std::int16_t rightX, std::int16_t rightY) {
    GamepadState state;
    state.standard = true;
    static constexpr struct {
        std::uint16_t bit;
        GamepadButton button;
    } kButtons[] = {
        {0x0001, GamepadButton::DpadUp},       {0x0002, GamepadButton::DpadDown},      {0x0004, GamepadButton::DpadLeft},
        {0x0008, GamepadButton::DpadRight},    {0x0010, GamepadButton::Start},         {0x0020, GamepadButton::Back},
        {0x0040, GamepadButton::LeftStick},    {0x0080, GamepadButton::RightStick},    {0x0100, GamepadButton::LeftShoulder},
        {0x0200, GamepadButton::RightShoulder}, {0x0400, GamepadButton::Guide},        {0x1000, GamepadButton::A},
        {0x2000, GamepadButton::B},            {0x4000, GamepadButton::X},             {0x8000, GamepadButton::Y},
    };
    for (const auto &entry : kButtons) {
        setButton(state, entry.button, (buttons & entry.bit) != 0);
    }
    // XInput's sticks are up-positive; the standard layout is down-positive.
    const auto stick = [](std::int16_t v) { return std::max(-1.0f, float(v) / 32767.0f); };
    setAxis(state, GamepadAxis::LeftX, stick(leftX));
    setAxis(state, GamepadAxis::LeftY, -stick(leftY));
    setAxis(state, GamepadAxis::RightX, stick(rightX));
    setAxis(state, GamepadAxis::RightY, -stick(rightY));
    setAxis(state, GamepadAxis::LeftTrigger, float(leftTrigger) / 255.0f);
    setAxis(state, GamepadAxis::RightTrigger, float(rightTrigger) / 255.0f);
    // Raw: the sixteen button bits and the six axes as the device has them.
    for (int bit = 0; bit < 16; ++bit) {
        state.rawButtons.push_back((buttons >> bit & 1) != 0);
    }
    state.rawAxes = {stick(leftX), stick(leftY), stick(rightX), stick(rightY), float(leftTrigger) / 127.5f - 1.0f,
                     float(rightTrigger) / 127.5f - 1.0f};
    return state;
}

// ---- Windows: HID pads that are not XInput ------------------------------------

// What a HID report said, by HID usage: the buttons in usage order (button 1
// first), the axes of the Generic Desktop page that the pad has, each already
// normalised to -1..1, and the hat switch (0 = up, clockwise to 7; anything
// else = centred).
struct HidPadReport {
    std::vector<bool> buttons;
    bool hasX = false, hasY = false, hasZ = false, hasRx = false, hasRy = false, hasRz = false, hasHat = false;
    float x = 0, y = 0, z = 0, rx = 0, ry = 0, rz = 0;
    int hat = -1;
};

inline constexpr std::uint16_t kVendorSony = 0x054C;

// Raw values for every HID pad; the standard layout for the pads whose
// report is known: Sony's (DualShock 4, DualSense), whose buttons run Square,
// Cross, Circle, Triangle, L1, R1, L2, R2, Share, Options, L3, R3, PS.
inline GamepadState stateFromHid(const HidPadReport &report, std::uint16_t vendorId) {
    GamepadState state;
    state.rawButtons = report.buttons;
    const bool up = report.hat == 7 || report.hat == 0 || report.hat == 1;
    const bool right = report.hat >= 1 && report.hat <= 3;
    const bool down = report.hat >= 3 && report.hat <= 5;
    const bool left = report.hat >= 5 && report.hat <= 7;
    for (const auto &[has, value] : {std::pair{report.hasX, report.x}, std::pair{report.hasY, report.y},
                                    std::pair{report.hasZ, report.z}, std::pair{report.hasRx, report.rx},
                                    std::pair{report.hasRy, report.ry}, std::pair{report.hasRz, report.rz}}) {
        if (has) {
            state.rawAxes.push_back(value);
        }
    }
    if (report.hasHat) {
        state.rawAxes.push_back(right ? 1.0f : left ? -1.0f : 0.0f);
        state.rawAxes.push_back(down ? 1.0f : up ? -1.0f : 0.0f);
    }
    if (vendorId != kVendorSony || report.buttons.size() < 13 || !report.hasZ || !report.hasRz) {
        return state;
    }
    state.standard = true;
    static constexpr GamepadButton kOrder[13] = {
        GamepadButton::X,     GamepadButton::A,         GamepadButton::B,          GamepadButton::Y,
        GamepadButton::LeftShoulder, GamepadButton::RightShoulder, GamepadButton::A /* L2: an axis */,
        GamepadButton::A /* R2 */,   GamepadButton::Back,      GamepadButton::Start,      GamepadButton::LeftStick,
        GamepadButton::RightStick,   GamepadButton::Guide};
    for (std::size_t i = 0; i < 13; ++i) {
        if (i != 6 && i != 7) {
            setButton(state, kOrder[i], report.buttons[i]);
        }
    }
    setButton(state, GamepadButton::DpadUp, up);
    setButton(state, GamepadButton::DpadDown, down);
    setButton(state, GamepadButton::DpadLeft, left);
    setButton(state, GamepadButton::DpadRight, right);
    // Left stick on X and Y, right stick on Z and Rz, the triggers on Rx and Ry.
    setAxis(state, GamepadAxis::LeftX, report.x);
    setAxis(state, GamepadAxis::LeftY, report.y);
    setAxis(state, GamepadAxis::RightX, report.z);
    setAxis(state, GamepadAxis::RightY, report.rz);
    setAxis(state, GamepadAxis::LeftTrigger, report.hasRx ? (report.rx + 1.0f) * 0.5f : (report.buttons[6] ? 1.0f : 0.0f));
    setAxis(state, GamepadAxis::RightTrigger, report.hasRy ? (report.ry + 1.0f) * 0.5f : (report.buttons[7] ? 1.0f : 0.0f));
    return state;
}

// ---- Linux: evdev ---------------------------------------------------------------

// One /dev/input/event* device seen as a pad. The backend tells it what the
// device has (its key codes and absolute axes with their ranges), then feeds
// it the device's events; state() is the pad at that moment.
class EvdevPad {
public:
    static constexpr int kEventKey = 1; // EV_KEY
    static constexpr int kEventAbs = 3; // EV_ABS
    static constexpr int kButtonSouth = 0x130; // BTN_SOUTH, also BTN_GAMEPAD
    static constexpr std::uint16_t kVendorNintendo = 0x057E;

    struct Axis {
        int code = 0;
        std::int32_t minimum = 0, maximum = 0, value = 0;
    };

    // `keys`: the key codes the device has, ascending. `axes`: its absolute
    // axes, ascending by code, with their ranges and current values.
    void configure(std::vector<int> keys, std::vector<Axis> axes, std::uint16_t vendorId) {
        m_keys = std::move(keys);
        m_axes = std::move(axes);
        m_down.assign(m_keys.size(), false);
        m_vendor = vendorId;
    }

    // The kernel describes it as a gamepad (it has the south button), as
    // opposed to a joystick, a wheel or something that is not a controller.
    [[nodiscard]] bool isGamepad() const noexcept { return hasKey(kButtonSouth); }
    // A controller of any kind: buttons from the joystick or gamepad ranges
    // and at least one axis.
    [[nodiscard]] bool isController() const noexcept {
        const bool buttons = std::any_of(m_keys.begin(), m_keys.end(), [](int k) { return k >= 0x120 && k < 0x140; });
        return buttons && !m_axes.empty();
    }

    void handle(int type, int code, std::int32_t value) noexcept {
        if (type == kEventKey) {
            const auto it = std::lower_bound(m_keys.begin(), m_keys.end(), code);
            if (it != m_keys.end() && *it == code) {
                m_down[std::size_t(it - m_keys.begin())] = value != 0;
            }
        } else if (type == kEventAbs) {
            for (Axis &axis : m_axes) {
                if (axis.code == code) {
                    axis.value = value;
                }
            }
        }
    }

    [[nodiscard]] GamepadState state() const {
        GamepadState state;
        state.rawButtons = m_down;
        for (const Axis &axis : m_axes) {
            state.rawAxes.push_back(normaliseAxis(axis.value, axis.minimum, axis.maximum));
        }
        if (!isGamepad()) {
            return state;
        }
        state.standard = true;
        // The kernel names the face buttons by position (south, east, north,
        // west), and Sony's and Nintendo's drivers follow that. The Xbox
        // driver, and the many pads that present themselves as Xbox pads,
        // report X as "north" and Y as "west": its X and Y are the codes'
        // old names, not their positions.
        const bool byPosition = m_vendor == kVendorSony || m_vendor == kVendorNintendo;
        setButton(state, GamepadButton::A, key(0x130));
        setButton(state, GamepadButton::B, key(0x131));
        setButton(state, GamepadButton::X, key(byPosition ? 0x134 : 0x133));
        setButton(state, GamepadButton::Y, key(byPosition ? 0x133 : 0x134));
        setButton(state, GamepadButton::LeftShoulder, key(0x136));
        setButton(state, GamepadButton::RightShoulder, key(0x137));
        setButton(state, GamepadButton::Back, key(0x13A));
        setButton(state, GamepadButton::Start, key(0x13B));
        setButton(state, GamepadButton::Guide, key(0x13C));
        setButton(state, GamepadButton::LeftStick, key(0x13D));
        setButton(state, GamepadButton::RightStick, key(0x13E));
        // The d-pad is a hat (two axes of -1, 0, 1) or four buttons.
        const float hatX = axis(0x10), hatY = axis(0x11);
        setButton(state, GamepadButton::DpadUp, key(0x220) || hatY < -0.5f);
        setButton(state, GamepadButton::DpadDown, key(0x221) || hatY > 0.5f);
        setButton(state, GamepadButton::DpadLeft, key(0x222) || hatX < -0.5f);
        setButton(state, GamepadButton::DpadRight, key(0x223) || hatX > 0.5f);
        setAxis(state, GamepadAxis::LeftX, axis(0x00));
        setAxis(state, GamepadAxis::LeftY, axis(0x01));
        setAxis(state, GamepadAxis::RightX, axis(0x03));
        setAxis(state, GamepadAxis::RightY, axis(0x04));
        // Triggers are the Z axes, or buttons on pads without analogue ones.
        setAxis(state, GamepadAxis::LeftTrigger, hasAxis(0x02) ? axis(0x02, true) : (key(0x138) ? 1.0f : 0.0f));
        setAxis(state, GamepadAxis::RightTrigger, hasAxis(0x05) ? axis(0x05, true) : (key(0x139) ? 1.0f : 0.0f));
        return state;
    }

private:
    [[nodiscard]] bool hasKey(int code) const noexcept { return std::binary_search(m_keys.begin(), m_keys.end(), code); }
    [[nodiscard]] bool key(int code) const noexcept {
        const auto it = std::lower_bound(m_keys.begin(), m_keys.end(), code);
        return it != m_keys.end() && *it == code && m_down[std::size_t(it - m_keys.begin())];
    }
    [[nodiscard]] bool hasAxis(int code) const noexcept {
        return std::any_of(m_axes.begin(), m_axes.end(), [code](const Axis &a) { return a.code == code; });
    }
    [[nodiscard]] float axis(int code, bool oneSided = false) const noexcept {
        for (const Axis &a : m_axes) {
            if (a.code == code) {
                return normaliseAxis(a.value, a.minimum, a.maximum, oneSided);
            }
        }
        return 0.0f;
    }

    std::vector<int> m_keys;
    std::vector<bool> m_down;
    std::vector<Axis> m_axes;
    std::uint16_t m_vendor = 0;
};

} // namespace cfw::detail
