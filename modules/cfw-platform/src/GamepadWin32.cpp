// Windows gamepads.
//
// Xbox-compatible pads through XInput, loaded at run time (xinput1_4.dll on
// Windows 8 and later, older names before): four slots, each read every
// poll() while it holds a pad and probed about once a second while empty
// (asking an empty slot is slow). The Guide button comes from the
// undocumented-but-stable XInputGetStateEx (ordinal 100) where the DLL has it.
//
// Everything else through Raw Input: a message-only window registered for
// the HID joystick and gamepad usages receives each device's reports and its
// arrival and removal, and hid.dll's parser (HidP_*) reads the buttons, axes
// and hat out of a report by HID usage. Devices that XInput also serves
// (their device path contains "IG_") are skipped here. Sony's pads get the
// standard layout (see GamepadMapping.h); any other HID pad is raw only.
//
// UNVERIFIED with hardware: the XInput path was run with a pad's worth of
// synthetic states in the tests and with no pad connected; the HID path
// compiles and enumerates, but no non-Xbox pad was at hand to press.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

extern "C" {
#include <hidsdi.h>
}

#include <algorithm>
#include <array>
#include <map>
#include <vector>

#include "GamepadMapping.h"
#include "cfw/core/Utf8.h"
#include "cfw/platform/Gamepad.h"

namespace cfw {

namespace {

// xinput.h's structures, declared here so no XInput import library is needed.
struct XGamepad {
    WORD buttons;
    BYTE leftTrigger;
    BYTE rightTrigger;
    SHORT leftX, leftY, rightX, rightY;
};
struct XState {
    DWORD packet;
    XGamepad pad;
};
struct XVibration {
    WORD leftMotor;  // low frequency
    WORD rightMotor; // high frequency
};

struct XInput {
    DWORD(WINAPI *getState)(DWORD, XState *) = nullptr;
    DWORD(WINAPI *setState)(DWORD, XVibration *) = nullptr;

    XInput() {
        for (const wchar_t *name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            if (const HMODULE library = LoadLibraryW(name)) {
                // By ordinal: the variant that also reports the Guide button.
                auto extended = reinterpret_cast<void *>(GetProcAddress(library, reinterpret_cast<LPCSTR>(100)));
                auto plain = reinterpret_cast<void *>(GetProcAddress(library, "XInputGetState"));
                getState = reinterpret_cast<DWORD(WINAPI *)(DWORD, XState *)>(extended ? extended : plain);
                setState = reinterpret_cast<DWORD(WINAPI *)(DWORD, XVibration *)>(
                    reinterpret_cast<void *>(GetProcAddress(library, "XInputSetState")));
                if (getState) {
                    return;
                }
            }
        }
    }
};

constexpr wchar_t kWindowClass[] = L"CfwGamepadInput";

struct HidDevice {
    GamepadId id = 0;
    std::uint16_t vendorId = 0;
    std::vector<std::uint8_t> preparsed;
    USHORT buttonCount = 0;
    struct Value {
        USAGE usage = 0;
        LONG minimum = 0, maximum = 0;
        USHORT bits = 0;
    };
    std::vector<Value> values;
};

class GamepadsWin32 final : public Gamepads {
public:
    GamepadsWin32() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = &GamepadsWin32::windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kWindowClass;
        RegisterClassExW(&wc); // fails harmlessly if a second object registered it already
        m_window = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (m_window) {
            SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            // Joysticks (usage 4) and gamepads (usage 5) of the Generic Desktop
            // page, in the background too, with arrival and removal notices
            // (which also come for the devices already there).
            RAWINPUTDEVICE devices[2] = {{1, 4, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, m_window},
                                         {1, 5, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, m_window}};
            RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
        }
    }

    ~GamepadsWin32() override {
        for (DWORD slot = 0; slot < 4; ++slot) {
            if (m_slots[slot].id && m_xinput.setState) {
                XVibration off{0, 0};
                m_xinput.setState(slot, &off);
            }
        }
        if (m_window) {
            RAWINPUTDEVICE devices[2] = {{1, 4, RIDEV_REMOVE, nullptr}, {1, 5, RIDEV_REMOVE, nullptr}};
            RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
            SetWindowLongPtrW(m_window, GWLP_USERDATA, 0);
            DestroyWindow(m_window);
        }
    }

protected:
    void readDevices() override {
        // HID: whatever arrived for the hidden window since the last poll.
        if (m_window) {
            MSG message;
            while (PeekMessageW(&message, m_window, 0, 0, PM_REMOVE)) {
                DispatchMessageW(&message);
            }
        }
        if (!m_xinput.getState) {
            return;
        }
        const DWORD now = GetTickCount();
        for (DWORD slot = 0; slot < 4; ++slot) {
            Slot &s = m_slots[slot];
            if (!s.id && s.probed && now - s.lastProbe < 1000 + slot * 100) {
                continue;
            }
            s.probed = true;
            s.lastProbe = now;
            XState state{};
            if (m_xinput.getState(slot, &state) != ERROR_SUCCESS) {
                if (s.id) {
                    deviceRemoved(s.id);
                    s.id = 0;
                }
                continue;
            }
            if (!s.id) {
                GamepadInfo info;
                info.name = "Xbox controller " + std::to_string(slot + 1);
                info.standard = true;
                info.canRumble = m_xinput.setState != nullptr;
                info.vendorId = 0x045E;
                s.id = deviceAdded(std::move(info));
            }
            const XGamepad &pad = state.pad;
            deviceState(s.id, detail::stateFromXInput(pad.buttons, pad.leftTrigger, pad.rightTrigger, pad.leftX, pad.leftY,
                                                      pad.rightX, pad.rightY));
        }
    }

    bool setMotors(GamepadId id, float lowFrequency, float highFrequency) override {
        for (DWORD slot = 0; slot < 4; ++slot) {
            if (m_slots[slot].id == id && m_xinput.setState) {
                XVibration vibration{WORD(lowFrequency * 65535.0f), WORD(highFrequency * 65535.0f)};
                return m_xinput.setState(slot, &vibration) == ERROR_SUCCESS;
            }
        }
        return false;
    }

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto *self = reinterpret_cast<GamepadsWin32 *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && message == WM_INPUT_DEVICE_CHANGE) {
            const auto device = reinterpret_cast<HANDLE>(lParam);
            if (wParam == GIDC_ARRIVAL) {
                self->hidArrived(device);
            } else {
                self->hidRemoved(device);
            }
            return 0;
        }
        if (self && message == WM_INPUT) {
            self->hidInput(reinterpret_cast<HRAWINPUT>(lParam));
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void hidArrived(HANDLE handle) {
        if (m_hid.count(handle)) {
            return;
        }
        RID_DEVICE_INFO deviceInfo{};
        deviceInfo.cbSize = sizeof deviceInfo;
        UINT size = sizeof deviceInfo;
        if (GetRawInputDeviceInfoW(handle, RIDI_DEVICEINFO, &deviceInfo, &size) == UINT(-1) ||
            deviceInfo.dwType != RIM_TYPEHID) {
            return;
        }
        std::wstring path;
        size = 0;
        GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, nullptr, &size);
        path.resize(size);
        if (size == 0 || GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, path.data(), &size) == UINT(-1)) {
            return;
        }
        path.resize(wcsnlen(path.c_str(), path.size()));
        if (path.find(L"IG_") != std::wstring::npos) {
            return; // an XInput device: served by the slots above
        }
        HidDevice device;
        device.vendorId = std::uint16_t(deviceInfo.hid.dwVendorId);
        size = 0;
        GetRawInputDeviceInfoW(handle, RIDI_PREPARSEDDATA, nullptr, &size);
        device.preparsed.resize(size);
        if (size == 0 || GetRawInputDeviceInfoW(handle, RIDI_PREPARSEDDATA, device.preparsed.data(), &size) == UINT(-1)) {
            return;
        }
        const auto parsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(device.preparsed.data());
        HIDP_CAPS caps{};
        if (HidP_GetCaps(parsed, &caps) != HIDP_STATUS_SUCCESS) {
            return;
        }
        std::vector<HIDP_BUTTON_CAPS> buttonCaps(caps.NumberInputButtonCaps);
        USHORT count = caps.NumberInputButtonCaps;
        if (count > 0 && HidP_GetButtonCaps(HidP_Input, buttonCaps.data(), &count, parsed) == HIDP_STATUS_SUCCESS) {
            for (USHORT i = 0; i < count; ++i) {
                if (buttonCaps[i].UsagePage == 0x09) {
                    const USAGE last = buttonCaps[i].IsRange ? buttonCaps[i].Range.UsageMax : buttonCaps[i].NotRange.Usage;
                    device.buttonCount = std::max<USHORT>(device.buttonCount, std::min<USAGE>(last, 128));
                }
            }
        }
        std::vector<HIDP_VALUE_CAPS> valueCaps(caps.NumberInputValueCaps);
        count = caps.NumberInputValueCaps;
        if (count > 0 && HidP_GetValueCaps(HidP_Input, valueCaps.data(), &count, parsed) == HIDP_STATUS_SUCCESS) {
            for (USHORT i = 0; i < count; ++i) {
                const HIDP_VALUE_CAPS &cap = valueCaps[i];
                const USAGE first = cap.IsRange ? cap.Range.UsageMin : cap.NotRange.Usage;
                const USAGE last = cap.IsRange ? cap.Range.UsageMax : cap.NotRange.Usage;
                for (USAGE usage = first; cap.UsagePage == 0x01 && usage <= last && usage >= first; ++usage) {
                    if ((usage >= 0x30 && usage <= 0x35) || usage == 0x39) {
                        device.values.push_back({usage, cap.LogicalMin, cap.LogicalMax, cap.BitSize});
                    }
                }
            }
        }
        GamepadInfo info;
        info.vendorId = device.vendorId;
        info.productId = std::uint16_t(deviceInfo.hid.dwProductId);
        info.standard = device.vendorId == detail::kVendorSony;
        info.name = "HID gamepad";
        // The product's own name, read from the device (no access rights
        // are needed for that).
        const HANDLE file = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            wchar_t product[128] = {};
            if (HidD_GetProductString(file, product, sizeof product - sizeof(wchar_t)) && product[0]) {
                info.name = utf16ToUtf8Lossy(
                    std::u16string_view(reinterpret_cast<const char16_t *>(product), wcsnlen(product, 127)));
            }
            CloseHandle(file);
        }
        device.id = deviceAdded(std::move(info));
        m_hid.emplace(handle, std::move(device));
    }

    void hidRemoved(HANDLE handle) {
        const auto found = m_hid.find(handle);
        if (found != m_hid.end()) {
            deviceRemoved(found->second.id);
            m_hid.erase(found);
        }
    }

    void hidInput(HRAWINPUT input) {
        UINT size = 0;
        if (GetRawInputData(input, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size < sizeof(RAWINPUTHEADER)) {
            return;
        }
        m_buffer.resize(size);
        if (GetRawInputData(input, RID_INPUT, m_buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size) {
            return;
        }
        const auto *raw = reinterpret_cast<const RAWINPUT *>(m_buffer.data());
        if (raw->header.dwType != RIM_TYPEHID) {
            return;
        }
        const auto found = m_hid.find(raw->header.hDevice);
        if (found == m_hid.end()) {
            return;
        }
        // The sizes are the system's, but check them against what was read.
        const std::size_t available = size - std::size_t(reinterpret_cast<const std::uint8_t *>(raw->data.hid.bRawData) - m_buffer.data());
        const std::size_t reportBytes = raw->data.hid.dwSizeHid;
        if (reportBytes == 0 || raw->data.hid.dwCount == 0 || reportBytes * raw->data.hid.dwCount > available) {
            return;
        }
        HidDevice &device = found->second;
        // Several reports may come at once: the last is the newest.
        auto *report = reinterpret_cast<PCHAR>(const_cast<BYTE *>(raw->data.hid.bRawData)) +
                       reportBytes * (raw->data.hid.dwCount - 1);
        const auto parsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(device.preparsed.data());
        detail::HidPadReport pad;
        pad.buttons.assign(device.buttonCount, false);
        USAGE pressed[128];
        ULONG pressedCount = 128;
        if (HidP_GetUsages(HidP_Input, 0x09, 0, pressed, &pressedCount, parsed, report, ULONG(reportBytes)) ==
            HIDP_STATUS_SUCCESS) {
            for (ULONG i = 0; i < pressedCount; ++i) {
                if (pressed[i] >= 1 && pressed[i] <= device.buttonCount) {
                    pad.buttons[std::size_t(pressed[i] - 1)] = true;
                }
            }
        }
        for (const HidDevice::Value &value : device.values) {
            ULONG bits = 0;
            if (HidP_GetUsageValue(HidP_Input, 0x01, 0, value.usage, &bits, parsed, report, ULONG(reportBytes)) !=
                HIDP_STATUS_SUCCESS) {
                continue;
            }
            // A negative logical minimum means the field is two's complement.
            std::int64_t number = bits;
            if (value.minimum < 0 && value.bits > 0 && value.bits < 32 && (bits >> (value.bits - 1)) & 1) {
                number -= std::int64_t(1) << value.bits;
            }
            if (value.usage == 0x39) {
                pad.hasHat = true;
                const std::int64_t step = number - value.minimum;
                pad.hat = (number >= value.minimum && number <= value.maximum && step < 8) ? int(step) : -1;
                continue;
            }
            const float axis = detail::normaliseAxis(number, value.minimum, value.maximum);
            switch (value.usage) {
            case 0x30: pad.hasX = true, pad.x = axis; break;
            case 0x31: pad.hasY = true, pad.y = axis; break;
            case 0x32: pad.hasZ = true, pad.z = axis; break;
            case 0x33: pad.hasRx = true, pad.rx = axis; break;
            case 0x34: pad.hasRy = true, pad.ry = axis; break;
            case 0x35: pad.hasRz = true, pad.rz = axis; break;
            default: break;
            }
        }
        deviceState(device.id, detail::stateFromHid(pad, device.vendorId));
    }

    struct Slot {
        GamepadId id = 0;
        bool probed = false;
        DWORD lastProbe = 0;
    };

    XInput m_xinput;
    std::array<Slot, 4> m_slots;
    HWND m_window = nullptr;
    std::map<HANDLE, HidDevice> m_hid;
    std::vector<std::uint8_t> m_buffer;
};

} // namespace

std::unique_ptr<Gamepads> Gamepads::create() { return std::make_unique<GamepadsWin32>(); }

} // namespace cfw
