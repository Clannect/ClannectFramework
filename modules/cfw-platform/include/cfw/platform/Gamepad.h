#pragma once

// Game controllers. One Gamepads object watches every pad the system has:
//
//     auto pads = Gamepads::create();
//     ScopedConnection onEvent = pads->event.connect([](const GamepadEvent &e) { ... });
//     for (;;) { processEvents(...); pads->poll(); ... }
//
// poll() reads the devices and, for everything that changed since the last
// poll, emits `connected`, `disconnected` and `event` on the calling thread;
// between polls nothing is emitted and state() does not change, so a frame
// sees one consistent picture. Events and polling are two views of the same
// thing: use either or both.
//
// Pads with a known layout are reported in the standard layout (GamepadState
// in cfw/core/Input.h: two sticks, two triggers, d-pad, face and shoulder
// buttons, stick clicks, Back, Start, Guide). Every pad also has its raw
// buttons and axes, which is all an unknown pad or a flight stick has.
// Values are raw and normalised; dead zones are the caller's.
//
// Backends:
//   Windows  XInput for Xbox-compatible pads (up to four), and Raw Input HID
//            for the others: PlayStation pads are mapped to the standard
//            layout, anything else is raw only.
//   Linux    evdev (/dev/input/event*), with hot-plug through inotify: no
//            udev. Pads the kernel describes as gamepads are standard.
//            Reading needs permission on the device nodes, which desktop
//            systems grant to the logged-in user.
//   macOS    the GameController framework (extended gamepads).
//   elsewhere, or where a backend finds nothing: no pads, and virtual pads
//            still work.
//
// Rumble is two motors, the heavy low-frequency one and the light
// high-frequency one, each 0 to 1, for a duration; it stops by itself.
// Windows (XInput pads) and Linux (pads with force feedback) have it.
//
// Virtual pads are pads the program feeds itself: for tests, replays and
// on-screen controls. They produce exactly the events a real pad would.
//
// Threads: one thread for everything, the one that calls poll() (on Windows
// the one that created the object too). Allocates: when a pad connects.

#include <memory>
#include <vector>

#include "cfw/core/Clock.h"
#include "cfw/core/Input.h"
#include "cfw/core/Signal.h"
#include "cfw/core/String.h"

namespace cfw {

class Gamepads {
public:
    // Never fails: a system with no backend or no permission has no pads.
    [[nodiscard]] static std::unique_ptr<Gamepads> create();
    virtual ~Gamepads();
    Gamepads(const Gamepads &) = delete;
    Gamepads &operator=(const Gamepads &) = delete;

    // Reads every pad and emits what changed. Call once a frame.
    void poll();

    // The pads connected as of the last poll, oldest first.
    [[nodiscard]] std::vector<GamepadInfo> pads() const;
    [[nodiscard]] bool isConnected(GamepadId id) const noexcept;
    // A pad's controls as of the last poll; a neutral state for an id that is
    // not connected.
    [[nodiscard]] const GamepadState &state(GamepadId id) const noexcept;

    // Runs the pad's two motors at `lowFrequency` and `highFrequency` (0 to
    // 1) for `duration`, replacing any rumble in progress; zeros stop it.
    // False if the pad has no rumble or is gone.
    bool rumble(GamepadId id, float lowFrequency, float highFrequency, Duration duration);

    Signal<const GamepadInfo &> connected;
    Signal<GamepadId> disconnected; // its state() is neutral from now on
    Signal<const GamepadEvent &> event;

    // A pad fed by the program. It appears (with `connected`) at the next
    // poll(), takes the state last set at each poll, and goes (with
    // `disconnected`) at the poll after removeVirtual().
    GamepadId addVirtual(String name, bool standard = true);
    void setVirtualState(GamepadId id, const GamepadState &state);
    void removeVirtual(GamepadId id);

protected:
    Gamepads();

    // ---- For backends ----
    // A device the backend found: returns the id it is known by from now on.
    GamepadId deviceAdded(GamepadInfo info);
    // What the device reports now; compared with the last poll's and turned
    // into events when poll() finishes.
    void deviceState(GamepadId id, GamepadState state);
    void deviceRemoved(GamepadId id);
    // Reads the backend's devices, calling the three functions above.
    virtual void readDevices() {}
    // Sets a device's motors; false if it cannot. Stopping after the
    // duration is done here, in poll().
    virtual bool setMotors(GamepadId id, float lowFrequency, float highFrequency) {
        (void)id, (void)lowFrequency, (void)highFrequency;
        return false;
    }

private:
    struct Pad;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace cfw
