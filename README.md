# Clannect Framework

**A C++20 application framework with no third-party dependencies.** Windows, text, images, sound, networking and a
full widget toolkit, written from scratch and tested against the reference implementations.

Clannect Framework (CFW) was built for [Clannect](https://github.com/Clannect): its game editor, game client and
headless game server run on CFW alone. Everything CFW needs, from PNG and MP3 decoding to OpenType shaping to TLS, is
implemented in this repository. It links only the C++ standard library and the operating system.

[![Rate CFW!](https://img.shields.io/badge/Rate%20CFW-⭐%201–5-gold?style=for-the-badge)](https://github.com/Clannect/ClannectFramework/discussions/2#discussion-10913620)


> **Status: pre-1.0 (0.2.0).** CFW runs Clannect today, but its API may still change between versions. Documentation is being written.

## Features

| Module | What it gives you |
|---|---|
| **cfw-core** | Strings and UTF-8/16, containers, math (vectors, quaternions, matrices), colours, `Variant`, reflection-lite properties, signals, `Result`/`Error`, logging, arenas, SHA-256, UUIDs, URLs, locale-aware number formatting, and DEFLATE/zlib compression. |
| **cfw-io** | Files and paths (UTF-8, atomic writes, memory mapping), JSON (strict, fast, with precise error positions), a binary reader/writer, zip archives, per-user settings and standard folders, processes. |
| **cfw-net** | An event loop, TCP, TLS 1.2/1.3 through the OS, an HTTP/1.1 client, and WebSocket client and server. |
| **cfw-image** | `Image`; PNG and JPEG decoding and encoding, and WebP decoding, exact to the pixel against the reference decoders; resizing and an atlas packer. |
| **cfw-audio** | Sound output through the system's default device (WASAPI, ALSA, CoreAudio), pulled by a callback at 48 kHz stereo float; WAV, Ogg Vorbis, MP3 and FLAC decoders, whole-file and streaming with seeking; an offline and a streaming resampler. See [Sound](#sound-cfw-audio). |
| **cfw-text** | Unicode 18 (grapheme clusters, line breaking, bidi), a TrueType/CFF font parser, OpenType shaping for every script HarfBuzz shapes (Arabic, Hebrew, Thai, Hangul, the Indic scripts, Khmer, Myanmar and the 90-odd scripts of the Universal Shaping Engine), text layout, font fallback and a glyph atlas. |
| **cfw-gfx** | A 2D `Painter`: paths, pens, dashes, gradients, images, transforms, clipping and blend modes, on an anti-aliased CPU rasteriser; SVG icons. |
| **cfw-platform** | Native windows (Win32, X11), keyboard (by layout and by position), mouse (with a relative mode for cameras), touch and pen, gamepads, input methods (IME), file drag and drop, the clipboard, cursors, high-DPI, and OpenGL 3.3 contexts with a built-in function loader. See [Input](#input). |
| **cfw-ui** | A retained widget toolkit: layout, focus, themes, buttons, text fields, menus, dropdowns, sliders, tree views, tabs, dockable panels, property grids, dialogs, a colour picker and more. |
| **cfw-app** | Puts it together: UI windows (CPU-painted, or composited over OpenGL views), screen reader support, native file and folder pickers, opening links. |

Modules are layered; each depends only on the ones above it in this table, so a headless server can link
cfw-core, cfw-io and cfw-net and nothing else. (cfw-audio needs cfw-core only.)

### Platforms

| Platform | Status |
|---|---|
| **Windows** (MinGW-w64 GCC 13) | Supported. |
| **Linux** (X11; GCC 13 or Clang 18) | Supported. |
| **macOS** 13.3+ (Apple silicon) | Experimental: builds and passes the tests on GitHub's macOS runners, but has not been tried on a real Mac by a person yet. |

## Quick start

### Build and run the tests

You need CMake 3.25 or newer, Ninja and a C++20 compiler. On Linux, install the X11 development headers
(`libx11-dev`) for windows; without them CFW builds a headless stub. Nothing else is needed to build. At run
time, sound uses `libasound.so.2` and relative mouse mode and touch use `libXi.so.6` if they are installed (they
are on every desktop system), and do without otherwise.

```sh
git clone https://github.com/Clannect/ClannectFramework.git
cd ClannectFramework
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Then try the widget gallery: `build/debug/examples/cfw-ui-gallery`.

### Use it in your project

With CMake's FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(
    ClannectFramework
    GIT_REPOSITORY https://github.com/Clannect/ClannectFramework.git
    GIT_TAG        main   # or a specific commit
)
FetchContent_MakeAvailable(ClannectFramework)

target_link_libraries(my_app PRIVATE cfw::app)   # or cfw::core, cfw::net, cfw::text...
```

As a subproject, CFW builds only its libraries (no tests, examples or benchmarks).

Or install a prebuilt version from [Releases](https://github.com/Clannect/ClannectFramework/releases):

- **Windows:** run the **online installer** (it downloads the version you choose and can tell you when a new one
  is released) or a version's **offline installer** (everything inside, no internet needed). Both can register the
  install with CMake, so `find_package` finds it with no path to set.
- **Linux x64, and macOS on Apple silicon (experimental):** unpack the package and point CMake at it with
  `-DCMAKE_PREFIX_PATH=<unpacked folder>`. A zip of the Windows package is there too.

```cmake
find_package(ClannectFramework 0.2 REQUIRED)
target_link_libraries(my_app PRIVATE cfw::app)
```

The packages also contain `cfw-ui-gallery`, a tour of the widgets.

### Hello, window

```cpp
#include <cstdio>
#include <string>

#include "cfw/app/UiWindow.h"
#include "cfw/ui/Controls.h"

using namespace cfw;

int main() {
    auto created = UiWindow::create({"Hello, CFW", {480, 320}}, Theme::dark().withSystemFonts());
    if (!created) {
        std::fprintf(stderr, "no window: %s\n", created.error().message().c_str());
        return 1;
    }
    UiWindow &window = *created.value();

    auto &column = window.surface().root().add<Stack>(Stack::Direction::Column, 12.0f, 24.0f);
    auto &label = column.add<Label>("Hello from Clannect Framework");
    auto &button = column.add<Button>("Click me");

    int clicks = 0;
    ScopedConnection onClick = button.clicked.connect([&] {
        label.setText("Clicked " + std::to_string(++clicks) + " times");
    });

    runUntilClosed(window);
}
```

CFW doesn't use exceptions for errors: fallible calls return `Result<T>`, which holds either a value or an
`Error`.

## Sound (cfw-audio)

```cpp
#include "cfw/audio/AudioDecoder.h"
#include "cfw/audio/AudioDevice.h"
#include "cfw/audio/Resampler.h"

// A clip: decode (the format is detected from the bytes), bring it to 48 kHz once.
auto clip = cfw::decodeAudio(bytes);                       // Result<AudioBuffer>: float, interleaved
auto ready = cfw::resample(clip.value(), cfw::AudioDevice::kSampleRate);

// The device pulls 48 kHz stereo float whenever it needs more.
auto device = cfw::AudioDevice::open([&](float *stereo, int frames) { mixer.mix(stereo, frames); });
if (device) device.value()->start();                       // no device: carry on silent
```

Long music is not decoded up front: `AudioStream::open(bytes)` reads a piece at a time and seeks, and
`StreamResampler` converts it block by block. `examples/play-audio` (`cfw-play-audio <file> [--stream]`) does both.

What works where. "Verified" means run and checked on that system; anything else is said.

| | Windows | Linux | macOS |
|---|---|---|---|
| WAV, FLAC decoders | Verified: bit-exact against files built in the tests and against the reference `flac` encoder's files. | Verified, also under ASan and UBSan. | Unverified: portable C++ that no Mac has compiled yet. |
| Ogg Vorbis decoder | Verified against FFmpeg's decoder on libvorbis' files (within 2e-5 a sample); exact length; seeking exact to the sample. | Same. | Unverified, as above. |
| MP3 decoder | Verified against FFmpeg's decoder on LAME's files, MPEG-1, 2 and 2.5 (within 1e-4 a sample), with gapless trimming and seeking. **Intensity stereo is implemented but unverified**: no encoder at hand produces it. Free-format streams are not supported. | Same. | Unverified, as above. |
| Resamplers | Verified against computed tones. | Verified. | Unverified, as above. |
| Output device | WASAPI shared mode. Verified on one Windows 11 machine: opened, started, pulled, stopped, destroyed while running (the test plays silence). **Unverified by hand:** following the default device when it is changed or unplugged, and devices that are not 48 kHz float stereo (the conversion itself is tested without hardware). | ALSA, loaded at run time. Verified in a container: loading `libasound`, the "no device" error, and the whole playback loop against ALSA's `null` device. **Unverified on real hardware**, and through PipeWire's or PulseAudio's ALSA plugin. | CoreAudio default output unit. **Unverified: written without a Mac, never compiled.** |
| Null device | Verified. | Verified. | Unverified, as above. |

Every decoder has a fuzz target (`FuzzWav`, `FuzzFlac`, `FuzzVorbis`, `FuzzMp3`, and `FuzzAudio` for the
format-detecting entry point). Not implemented: Vorbis floor type 0 (pre-2001 encoders), Ogg files with several
logical streams (the first is played), MPEG Layer I and II, and channel layouts beyond passing every channel through.

## Input

```cpp
window->setRelativeMouse(true);                            // first-person camera: raw deltas, cursor hidden
ScopedConnection c = window->pointer.connect([&](const cfw::PointerEvent &e) { camera.turn(e.delta); });

auto pads = cfw::Gamepads::create();                       // then pads->poll() once a frame
```

- **Relative mouse mode** (`Window::setRelativeMouse`): the cursor is hidden and held, and `PointerEvent::delta`
  carries the mouse's own movement, unaccelerated and not stopped by the screen's edge. It ends when the window loses
  the focus and says so (`relativeMouseChanged`). `Window::setCursorConfined` keeps a visible cursor in the window.
- **Keys** carry both `key` (what the user's layout produces: what shortcuts mean) and `physicalKey` (the position, by
  its US-layout name: what W, A, S, D mean). Left and right modifiers are told apart in `physicalKey`
  (`Key::LeftShift`, `Key::RightShift`...); `key` stays `Key::Shift`, so existing comparisons keep working.
- **Touch and pen**: `Window::touch` reports every contact with an id; the primary one also arrives on
  `Window::pointer` as the left mouse button, so code written for the mouse works unchanged.
- **Gamepads** (`cfw/platform/Gamepad.h`): the standard layout plus raw buttons and axes, events and polling, rumble,
  and virtual pads for tests.

`examples/input-events` (`cfw-input-events`) prints every event; run it with a real mouse, keyboard and pad.

| | Windows | Linux (X11) | macOS |
|---|---|---|---|
| Relative mouse mode | Raw Input. Verified on Windows 11 with injected mouse movement (in `WindowTest`). | XInput 2 raw motion (libXi loaded at run time). Verified under Xvfb with input injected through XTEST (`X11InputTest`). **Unverified on a physical X server** and under XWayland. | `CGAssociateMouseAndMouseCursorPosition`; the deltas are after the system's acceleration. **Unverified: never compiled.** |
| Confined cursor | Verified. | Verified under Xvfb. | Not implemented. |
| Key position, left/right modifiers | Verified with posted key messages. | Verified under Xvfb (positions from XKB key names). | **Unverified.** |
| Touch and pen | `WM_POINTER`. The routing is tested; **the Windows code is unverified**: no touch screen or pen was at hand. | XInput 2.2 touch. **Unverified** (Xvfb has no touch device). | Not implemented (no touch screens; a trackpad is a mouse). |
| Gamepads | XInput, and Raw Input HID for other pads (Sony's mapped, others raw). The mappings are tested; **unverified with hardware**: no pad was connected. | evdev with inotify hot-plug, rumble through force feedback. The mapping is tested; **unverified with hardware.** | GameController framework, no rumble. **Unverified: never compiled.** |

There is no Wayland backend: on a Wayland desktop CFW runs through XWayland.

## Build presets

| Preset | Use |
|---|---|
| `debug` | Everyday development: contract checks and bounds-checked containers. |
| `release` | Optimised, checks off. |
| `asan` | AddressSanitizer + UndefinedBehaviorSanitizer + leak checks. |
| `tsan` | ThreadSanitizer (Linux). |
| `ubsan-trap` | UndefinedBehaviorSanitizer without a runtime; works with MinGW. |
| `fuzz` | Clang + libFuzzer fuzz targets for every parser. |

To build for Windows from Linux: `cmake -S . -B build/windows -G Ninja
-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-cross.cmake`. The tests run under Wine.

## How it's tested

CFW replaces libraries that have had decades of hardening, so it is tested against them directly:

- **Reference outputs.** PNG decoding is exact on all of PngSuite; JPEG and WebP decode to exactly
  libjpeg-turbo's and libwebp's pixels. FLAC decodes bit for bit; Vorbis and MP3 match FFmpeg's decoders
  sample by sample to float rounding. Font parsing
  matches fontTools and FreeType glyph for glyph. Shaping matches HarfBuzz on every glyph id, cluster and
  offset. Text algorithms pass all of Unicode's conformance tests. The references run outside the build to
  produce expected results; they are never linked.
- **Fuzzing.** Every parser (JSON, URLs, HTTP, WebSocket frames, compression, PNG, JPEG, WebP, the four sound
  formats, fonts, SVG, shaping, painting) has a libFuzzer target, and the committed corpus replays in every build.
- **Sanitizers.** CI runs the whole suite under ASan, UBSan and LSan, and the networking code under TSan.
- **Real clients.** Input methods, screen reader support and drag and drop are tested against the real system
  components (an X input method, the AT-SPI registry, and Windows' own APIs under Wine); keyboard and mouse input
  is injected through the X server (XTEST) and the Windows input stack.
- **Soak tests.** Long randomised runs of the network stack and the widget toolkit; `CFW_SOAK_SECONDS` makes
  them run longer.

Warnings are errors in CFW's own code.

## Repository layout

```
modules/<module>/include/cfw/<name>/   public headers
modules/<module>/src/                  implementation
modules/<module>/tests/                tests, one executable per area
examples/                              example programs (the widget gallery, cfw-play-audio, cfw-input-events)
tools/installer/                       the Windows installer (online, offline, maintenance tool)
fuzz/                                  fuzz targets and their corpus
bench/                                 benchmarks
testing/                               test helpers and the scripts that produce reference results
docs/decisions/                        design decisions, one file each
```

## Design decisions

Why things are the way they are is recorded in [`docs/decisions`](docs/decisions): for example why
[`String` is `std::string`](docs/decisions/0001-string-is-std-string.md), why CFW
[writes its own codecs](docs/decisions/0012-cfw-is-independent-own-codecs.md) and
[text stack](docs/decisions/0015-cfw-text-own-unicode-fonts-shaping.md), how
[TLS goes through the operating system](docs/decisions/0013-tls-through-the-os-and-windows-ci.md), and how
[sound](docs/decisions/0018-cfw-audio.md) and [game input](docs/decisions/0019-game-input.md) were added.

## Contributing

Issues and pull requests are welcome. Please keep to the style of the surrounding code, add tests with
your change, and make sure `ctest --preset debug` passes with no warnings.

## License

Clannect Framework is available under the [Open Use License (OUL) v1.1](license.md). You may use, modify and
distribute it, commercially too, as long as you keep the license and copyright notice, credit Clannect with a
link to this repository, and mark modified versions as modified. See [license.md](license.md) for the full
terms.

Test data from third parties keeps its own license: the DejaVu fonts, PngSuite and the Unicode test files
(see the `testdata` folders). The sound test files are CFW's own: synthetic tones encoded by
`testing/audio-oracle/make_audio_testdata.sh`.
