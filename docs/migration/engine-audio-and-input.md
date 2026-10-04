# Migrating Clannect Engine to cfw-audio and the new input API

For the engine's developers. What to delete, what to call instead, and the few things that behave differently.
Nothing here is required at once: every existing call still compiles and does what it did.

## 1. Sound

Link `cfw::audio` (it needs `cfw::core` only).

**Delete** `engine/src/audio/AudioClip.*` and `engine/src/audio/AudioDevice.*`. **Keep** `AudioMixer`.

| Engine code today | Replacement |
|---|---|
| The engine's WAV decoder in `AudioClip` | `cfw::decodeAudio(bytes, limits)` → `Result<AudioBuffer>`. It detects WAV, Ogg Vorbis, MP3 and FLAC from the bytes. |
| Converting a clip to the mixer's rate | `cfw::resample(buffer, 48000)`, once, at load. |
| Downmixing | Still the engine's: `AudioBuffer::samples` holds every channel, interleaved. |
| `AudioDevice` (waveOut) | `cfw::AudioDevice::open(callback)`, then `start()`. |
| Silent on Linux and macOS | The same call; ALSA and CoreAudio backends (see README for what is verified). |

```cpp
#include "cfw/audio/AudioDecoder.h"
#include "cfw/audio/AudioDevice.h"
#include "cfw/audio/Resampler.h"

// Loading a clip (bytes from a MappedFile or the asset cache).
cfw::AudioLimits limits;                         // caps the decoded size: set maxDecodedBytes to taste
auto decoded = cfw::decodeAudio(bytes, limits);
if (!decoded) { log(decoded.error().describe()); return; }
auto clip = cfw::resample(decoded.value(), cfw::AudioDevice::kSampleRate);
// clip.value().samples: float, interleaved, clip.value().channels channels, 48 kHz.

// The device. AudioMixer::mix(float *stereo, int frames) is already the right shape.
auto device = cfw::AudioDevice::open([this](float *stereo, int frames) { m_mixer.mix(stereo, frames); });
if (device) {
    m_device = std::move(device).value();
    m_device->start();
} else {
    log(device.error().describe());              // no output device: carry on silent
}
```

Things to know:

- The callback runs on the device's thread, as before. It must not block or allocate. `frames` varies from call
  to call (1 to `AudioDevice::kMaxCallbackFrames`); the buffer arrives zeroed.
- `stop()` and the destructor return only when the callback is not running and will not run again. Do not call
  them from the callback.
- The default device is followed without a call from the engine. While there is no device the callback is still
  pulled at the right rate, so the mixer's clock keeps moving.
- `latency()` is what the system gave; use it to line sound up with picture.
- For tests and the headless server: `cfw::AudioDevice::openNull(callback)`, or `openNull(callback, false)` and
  `render(frames)` to pull by hand.
- Music: do not decode it whole. `cfw::AudioStream::open(bytes)` gives `read()`, `seek()` and `totalFrames()`;
  decode on a worker thread into a queue the mixer reads, converting with `cfw::StreamResampler` if the file is
  not 48 kHz. `examples/play-audio` is a complete, small example of both paths.

## 2. The camera: relative mouse mode

Replace "hide the cursor, warp it to the middle, measure the distance" with:

```cpp
window.setRelativeMouse(true);                   // when the camera takes the mouse (right button down, or focus)
// in the pointer handler:
if (e.type == cfw::PointerEvent::Type::Move && window.isRelativeMouse()) {
    camera.turn(e.delta.x * sensitivity, e.delta.y * sensitivity);   // raw counts: right and down positive
}
window.setRelativeMouse(false);                  // when it lets go
```

- Remove the `setCursor(Cursor::Hidden)` and `setPointerPosition()` calls that did this before. Both still exist
  and work; `setPointerPosition` is no longer the way to do a camera.
- `e.delta` is in the mouse's own counts, not pixels, and is not accelerated: the sensitivity constant will need
  retuning once (a typical mouse gives 800 to 1600 counts an inch).
- `e.position` does not move while the mode is on, and the cursor is back where it was when it ends.
- The mode ends when the window loses the focus. Connect `window.relativeMouseChanged` to update the camera's
  state, and turn the mode on again when the user comes back and asks for it; it does not return by itself.
- `setRelativeMouse(true)` does nothing if the window is not focused: check `isRelativeMouse()`.
- `window.setCursorConfined(true)` keeps a visible cursor inside the window.

## 3. Keys

```cpp
// Movement: by position, so it is the same four keys on AZERTY, QWERTZ and Dvorak.
if (e.physicalKey == cfw::Key::W) ...
// Shortcuts: by what the layout produces, as before.
if (e.key == cfw::Key::Z && hasModifier(e.modifiers, cfw::Modifier::Control)) ...
// Left and right modifiers.
if (e.physicalKey == cfw::Key::LeftShift) ...  else if (e.physicalKey == cfw::Key::RightShift) ...
```

- `e.key` means what it meant on Windows and Linux: the layout's key, with `Key::Shift`, `Key::Control`,
  `Key::Alt` and `Key::Meta` for either side. Existing comparisons keep working.
- `e.physicalKey` is new. It is `Key::Unknown` in events the engine builds itself without setting it.
- `cfw::eitherSide(Key::LeftShift) == Key::Shift`, for code that gets a sided key and wants the old one.
- On macOS `e.key` used to be the position; it is now the layout's key, as on the other platforms.

## 4. Gamepads

```cpp
#include "cfw/platform/Gamepad.h"

m_pads = cfw::Gamepads::create();
m_padEvents = m_pads->event.connect([this](const cfw::GamepadEvent &e) { ... });
// every frame, after processEvents():
m_pads->poll();
const cfw::GamepadState &pad = m_pads->state(id);
float x = pad.axis(cfw::GamepadAxis::LeftX);     // raw: apply the engine's own dead zone
```

`pads()` lists what is connected; `connected` and `disconnected` announce changes; `rumble(id, low, high,
duration)` returns false where the pad cannot. A pad whose `standard` is false has only `rawButtons` and
`rawAxes`. `addVirtual()` makes a pad the engine's tests can drive.

## 5. Touch

`window.touch` reports every finger and pen with `id`, `kind`, `primary` and `pressure`. Code that only handles
the mouse needs no change: the primary contact arrives on `window.pointer` as the left button.

## What can break a build

Nothing was removed or renamed, but two enums grew, which matters to a `switch` with no `default` when warnings
are errors (`-Werror=switch`):

- `cfw::PointerEvent::Type` gained `Cancel`. It is only ever sent for touch and pen contacts the system takes
  away. Treat it as a release that must not click.
- `cfw::Key` gained `LeftShift`, `RightShift`, `LeftControl`, `RightControl`, `LeftAlt`, `RightAlt`, `LeftMeta`
  and `RightMeta`, after the existing keys (whose values did not change). They appear in `physicalKey` only.

`PointerEvent`, `KeyEvent` and `Window` gained members at the end; aggregate initialisation of the old members
is unaffected.

## What behaves differently

- **Touch on Windows.** Windows used to turn the primary touch into mouse messages itself. CFW now handles the
  touch and produces the same Press, Move and Release on `pointer` (marked `kind == Touch`). A cancelled touch
  arrives as `Cancel` instead of whatever Windows synthesised.
- **`Surface::startTimer` with `repeat`.** Unchanged, but now specified (see `Surface.h`): a callback slower than
  its interval runs once per `runTimers()`, never back to back to catch up, and never inside another timer.
  The engine's 16 ms tick and 33 ms brush timer are each late when the other is slow, and neither is skipped.
- **`writeFileAtomic`.** Unchanged, and confirmed safe when the target is on another volume than the temporary
  directory: its temporary file is the target's sibling, and the temporary directory is never used.
