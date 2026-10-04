# 0019 — Game input: relative mouse, key positions, touch, gamepads

**Status:** accepted, 2026-10-04. Clannect Engine's first-person camera hid the cursor, warped it to the middle
of the window after every move and measured the distance. That loses movement at speed and fights devices that
report positions (tablets, remote desktops). It also could not tell left Shift from right, nor W from Z on an
AZERTY keyboard, and had no pads or touch.

## What was built

| Piece | How |
|---|---|
| Relative mouse mode | `Window::setRelativeMouse`. Windows: Raw Input (`WM_INPUT`) with the cursor clipped to the pixel it is on. X11: the pointer grabbed with an invisible cursor and XInput 2 raw motion selected on the root window; libXi is loaded by `dlopen` and its structures declared by hand. macOS: `CGAssociateMouseAndMouseCursorPosition`. Movement arrives in `PointerEvent::delta`; `position` does not move. |
| Confined cursor | `Window::setCursorConfined`: `ClipCursor` to the client area; a confining pointer grab on X11. Lifted while unfocused. |
| Key positions | `KeyEvent::physicalKey` beside `key`. Windows: the message's scan code. X11: the name XKB gives the key code ("AD02"), falling back to evdev numbering. macOS: the virtual key code, which is a position already. |
| Left and right modifiers | `Key::LeftShift` ... `Key::RightMeta`, reported in `physicalKey`. `key` stays `Key::Shift` and so on. |
| Touch and pen | `Window::touch` carries every contact with an id; `Window::deliverTouch` also sends the primary one to `Window::pointer` as the left mouse button. Windows: `WM_POINTER`. X11: XInput 2.2. |
| Gamepads | `Gamepads` (cfw/platform/Gamepad.h): `poll()` once a frame emits connect, disconnect and change events and updates `state()`. Windows: XInput plus Raw Input HID. Linux: evdev with inotify. macOS: GameController. |

## Decisions

- **`key` is the layout's key; `physicalKey` is the position.** Before this, `key` was the layout's on Windows
  and X11 and the position on macOS, and the header called it "physical". The two meanings are now two fields
  with one definition each on every platform; macOS's `key` changes to match the others.
- **Sided modifiers only in `physicalKey`.** If `key` became `LeftShift`, every `e.key == Key::Shift` in the
  engine and in cfw-ui would silently stop matching. `eitherSide()` folds a sided key back.
- **Two pointer signals.** If every finger arrived on `pointer`, code written for one mouse would see a second
  finger as the mouse jumping. So `pointer` stays one pointer (the mouse, or the primary contact acting as it)
  and `touch` is the many. A contact the system takes away is `Cancel`, which cfw-ui ends as a press without a click.
- **Relative mode ends with the focus and does not return by itself**, as pointer lock does in a browser: the
  application decides when to take the mouse again.
- **Gamepads are polled, and events come out of the poll.** One consistent picture a frame, on the caller's
  thread, with no locking. Raw normalised values; dead zones are the caller's.
- **`standard` is a promise.** A pad gets the standard layout only where the mapping is known (XInput; Sony over
  HID; what the kernel calls a gamepad on Linux; the system's extended profile on macOS). An unknown HID pad is
  raw only rather than guessed at.
- **The logic is separate from the system calls** (`KeyCodes.h`, `RelativeMotion.h`, `GamepadMapping.h`), so each
  platform's mapping is tested on every platform, and virtual pads and `deliverTouch` give tests the same path
  real devices take.

## Rejected

- **Windows.Gaming.Input.** It needs WinRT activation from MinGW for what Raw Input HID gives with two DLLs
  already on every Windows.
- **udev for hot-plug.** inotify on `/dev/input` is the kernel's own and needs no library.
- **A database of pad mappings (SDL's).** Third-party data, and large. Raw buttons and axes let the engine offer
  binding by hand for pads CFW does not know.

## Not done

- Wayland (relative-pointer, pointer-constraints): CFW has no Wayland backend; XWayland is used.
- Touch on macOS (there are no touch screens), pen tilt and barrel buttons, touch pressure on X11.
- Rumble on macOS (CoreHaptics) and on HID pads on Windows.
- The number pad, Caps Lock and the other keys `Key` has no name for.

## Verification: an honest limit

Windows and X11 window code is verified by tests that inject input the way hardware delivers it (the Windows
input stack; XTEST under Xvfb). No touch screen, pen or gamepad was available: their system-facing code compiles
and their mapping logic is tested with synthetic input, but nothing was pressed. The macOS additions have not
been compiled. README's table says which is which.
