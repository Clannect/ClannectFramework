# 0016 — macOS: AppKit from Objective-C++, verified only by CI

**Status:** accepted, 2026-09-29. Spec §10, M6: "Linux and macOS builds".

## What was built

| Piece | How |
|---|---|
| Windows (`WindowCocoa.mm`) | An `NSWindow` per `cfw::Window` with one content view (flipped, y down). The view is the `NSTextInputClient` (text and input methods: `setMarkedText` becomes `CompositionEvent`, `insertText` becomes `TextEvent`) and the dragging destination (file URLs become `DropEvent`s, with paths from Enter on, as on Windows). |
| Presenting | `present()` turns the frame into a `CGImage` drawn by `drawRect:`. `capture()` returns the last frame presented: reading the screen back would need the screen-recording permission. |
| Event loop | `processEvents()` pumps `NSApp`'s queue with `nextEventMatchingMask:untilDate:` (no `[NSApp run]`), so applications keep their own loop as on Windows and X11. `wakeUp()` posts an application-defined event, which AppKit allows from any thread. |
| GL (`GlContextCocoa.mm`) | `NSOpenGLContext`. Apple's OpenGL is deprecated but shipped, and stops at 4.1 core: a 3.3 core request gets 4.1. Functions come from the OpenGL framework through `dlsym`. |
| Clipboard | `NSPasteboard`. |
| Desktop (`DesktopCocoa.mm`) | `NSOpenPanel` for the picker (patterns such as `*.png` become content types), `NSWorkspace` for Finder and links. |
| TLS | The OpenSSL backend, loading Homebrew's or MacPorts' `libssl.3.dylib` when present. macOS has no system OpenSSL; without one, TLS reports Unsupported. |
| `std::from_chars` | Apple's libc++ has no floating-point `from_chars` before LLVM 20. `cfw::fromChars` uses the standard one where the library has it, and elsewhere a strict `strtod_l` in the C locale on the longest valid prefix. That fallback is built and tested on Linux against `std::from_chars` (200,000 random inputs plus edge cases, bit for bit). |

Command reports as `Modifier::Control` and the Control key as `Modifier::Meta`, as Qt did, so
"Ctrl+S" shortcuts are Command-S. The minimum is macOS 13.3, the first whose libc++ has floating-point
`to_chars`.

## Rejected

- **Calling AppKit from plain C++ through `objc_msgSend`**, which would let the file compile on Linux.
  Rejected because the compiler would check none of the selectors or types. The code would compile
  here and still be wrong on a Mac.
- **Metal, with a GL-over-Metal layer.** The engine's renderer is GL 3.3, and Apple's GL still runs it.
  Moving to Metal belongs with the GPU paint backend, not this port.
- **osascript for the file picker.** A separate process shows the dialog, so it isn't owned by the
  application and can come up behind its windows.

## Not done yet

- **Accessibility.** macOS uses the no-op bridge. The cfw-ui tree maps directly onto
  `NSAccessibilityElement`, but that is future work.
- **Diagonal-resize and busy cursors.** AppKit has no public ones, so the arrow is shown instead.

## Verification: an honest limit

This code was written without a Mac. The container that built CFW has no macOS SDK, so nothing in
`*.mm` has been compiled. The `macos` workflow (`.github/workflows/macos.yml`) builds and runs every test
on GitHub's macOS runners. Until it has passed, treat the backend as **unverified**. Expect the first run
to find compile errors: the code follows AppKit's documented API, but no compiler has checked it.
