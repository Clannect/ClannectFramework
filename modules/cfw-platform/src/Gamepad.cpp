// What every platform's gamepads share: the list of pads, turning each
// poll's states into events, rumble that stops itself, and virtual pads.

#include "cfw/platform/Gamepad.h"

#include <algorithm>

namespace cfw {

struct Gamepads::Pad {
    GamepadInfo info;
    GamepadState state;   // as of the last poll: what state() returns
    GamepadState pending; // what the device reported since
    bool hasPending = false;
    bool isVirtual = false;
    bool announced = false;
    bool removed = false;
    bool rumbling = false;
    TimePoint rumbleEnd{};
};

struct Gamepads::Impl {
    std::vector<Pad> pads;
    GamepadId nextId = 1;
    GamepadState neutral;

    Pad *find(GamepadId id) noexcept {
        for (Pad &pad : pads) {
            if (pad.info.id == id) {
                return &pad;
            }
        }
        return nullptr;
    }
};

Gamepads::Gamepads() : m_impl(std::make_unique<Impl>()) {}
Gamepads::~Gamepads() = default;

GamepadId Gamepads::deviceAdded(GamepadInfo info) {
    info.id = m_impl->nextId++;
    Pad pad;
    pad.info = std::move(info);
    m_impl->pads.push_back(std::move(pad));
    return m_impl->pads.back().info.id;
}

void Gamepads::deviceState(GamepadId id, GamepadState state) {
    if (Pad *pad = m_impl->find(id); pad && !pad->removed) {
        pad->pending = std::move(state);
        pad->hasPending = true;
    }
}

void Gamepads::deviceRemoved(GamepadId id) {
    if (Pad *pad = m_impl->find(id)) {
        pad->removed = true;
    }
}

GamepadId Gamepads::addVirtual(String name, bool standard) {
    GamepadInfo info;
    info.name = std::move(name);
    info.standard = standard;
    const GamepadId id = deviceAdded(std::move(info));
    m_impl->find(id)->isVirtual = true;
    return id;
}

void Gamepads::setVirtualState(GamepadId id, const GamepadState &state) {
    if (Pad *pad = m_impl->find(id); pad && pad->isVirtual) {
        deviceState(id, state);
        pad->pending.standard = pad->info.standard;
    }
}

void Gamepads::removeVirtual(GamepadId id) {
    if (Pad *pad = m_impl->find(id); pad && pad->isVirtual) {
        pad->removed = true;
    }
}

std::vector<GamepadInfo> Gamepads::pads() const {
    std::vector<GamepadInfo> list;
    for (const Pad &pad : m_impl->pads) {
        if (pad.announced) {
            list.push_back(pad.info);
        }
    }
    return list;
}

bool Gamepads::isConnected(GamepadId id) const noexcept {
    const Pad *pad = m_impl->find(id);
    return pad && pad->announced;
}

const GamepadState &Gamepads::state(GamepadId id) const noexcept {
    const Pad *pad = m_impl->find(id);
    return pad && pad->announced ? pad->state : m_impl->neutral;
}

bool Gamepads::rumble(GamepadId id, float lowFrequency, float highFrequency, Duration duration) {
    Pad *pad = m_impl->find(id);
    if (!pad || !pad->announced || pad->removed || !pad->info.canRumble) {
        return false;
    }
    const float low = std::clamp(lowFrequency, 0.0f, 1.0f);
    const float high = std::clamp(highFrequency, 0.0f, 1.0f);
    const bool on = (low > 0.0f || high > 0.0f) && duration > Duration::zero();
    if (!setMotors(id, on ? low : 0.0f, on ? high : 0.0f)) {
        return false;
    }
    pad->rumbling = on;
    pad->rumbleEnd = Clock::now() + duration;
    return true;
}

void Gamepads::poll() {
    readDevices();
    // Handlers may add and remove pads and start rumbles, so the list is
    // never held across a signal: each pad is found again by its id.
    std::vector<GamepadId> ids;
    for (const Pad &pad : m_impl->pads) {
        ids.push_back(pad.info.id);
    }
    const TimePoint now = Clock::now();
    for (const GamepadId id : ids) {
        Pad *pad = m_impl->find(id);
        if (!pad) {
            continue;
        }
        if (!pad->announced) {
            if (pad->removed) {
                continue; // came and went between two polls
            }
            pad->announced = true;
            const GamepadInfo info = pad->info;
            connected.emit(info);
            pad = m_impl->find(id);
            if (!pad) {
                continue;
            }
        }
        if (pad->rumbling && now >= pad->rumbleEnd) {
            pad->rumbling = false;
            setMotors(id, 0.0f, 0.0f);
        }
        if (pad->hasPending && !pad->removed) {
            pad->hasPending = false;
            // Publish the new state first, so a handler asking state() from
            // inside an event sees what the event describes.
            GamepadState before = std::move(pad->state);
            pad->state = pad->pending;
            const GamepadState after = pad->state;
            std::vector<GamepadEvent> events;
            GamepadEvent e;
            e.id = id;
            if (after.standard) {
                for (std::size_t b = 0; b < kGamepadButtonCount; ++b) {
                    if (after.buttons[b] != before.buttons[b]) {
                        e.type = after.buttons[b] ? GamepadEvent::Type::ButtonPress : GamepadEvent::Type::ButtonRelease;
                        e.button = static_cast<GamepadButton>(b);
                        e.value = after.buttons[b] ? 1.0f : 0.0f;
                        events.push_back(e);
                    }
                }
                for (std::size_t a = 0; a < kGamepadAxisCount; ++a) {
                    if (after.axes[a] != before.axes[a]) {
                        e.type = GamepadEvent::Type::AxisChange;
                        e.axis = static_cast<GamepadAxis>(a);
                        e.value = after.axes[a];
                        events.push_back(e);
                    }
                }
            }
            e.button = GamepadButton::A;
            e.axis = GamepadAxis::LeftX;
            for (std::size_t b = 0; b < after.rawButtons.size(); ++b) {
                const bool was = b < before.rawButtons.size() && before.rawButtons[b];
                if (after.rawButtons[b] != was) {
                    e.type = after.rawButtons[b] ? GamepadEvent::Type::RawButtonPress : GamepadEvent::Type::RawButtonRelease;
                    e.rawIndex = int(b);
                    e.value = after.rawButtons[b] ? 1.0f : 0.0f;
                    events.push_back(e);
                }
            }
            for (std::size_t a = 0; a < after.rawAxes.size(); ++a) {
                const bool known = a < before.rawAxes.size();
                if (!known || after.rawAxes[a] != before.rawAxes[a]) {
                    e.type = GamepadEvent::Type::RawAxisChange;
                    e.rawIndex = int(a);
                    e.value = after.rawAxes[a];
                    // An axis seen for the first time at rest is not a change.
                    if (known || after.rawAxes[a] != 0.0f) {
                        events.push_back(e);
                    }
                }
            }
            for (const GamepadEvent &one : events) {
                event.emit(one);
            }
            pad = m_impl->find(id);
            if (!pad) {
                continue;
            }
        }
        if (pad->removed) {
            const bool announced = pad->announced;
            m_impl->pads.erase(m_impl->pads.begin() + (pad - m_impl->pads.data()));
            if (announced) {
                disconnected.emit(id);
            }
        }
    }
}

} // namespace cfw
