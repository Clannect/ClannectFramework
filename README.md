# Clannect Framework

**A C++20 application framework with no third-party dependencies.** Windows, text, images, networking and a full
widget toolkit, written from scratch and tested against the reference implementations.

Clannect Framework (CFW) was built for [Clannect](https://github.com/Clannect): its game editor, game client and
headless game server run on CFW alone. Everything CFW needs, from PNG decoding to OpenType shaping to TLS, is implemented
in this repository. It links only the C++ standard library and the operating system.

[![Rate CFW!](https://img.shields.io/badge/Rate%20CFW-⭐%201–5-gold?style=for-the-badge)](https://github.com/Clannect/ClannectFramework/discussions/2#discussion-10913620)


> **Status: pre-1.0 (0.1.0).** CFW runs Clannect today, but its API may still change between versions. Documentation is being written.

## Features

| Module | What it gives you |
|---|---|
| **cfw-core** | Strings and UTF-8/16, containers, math (vectors, quaternions, matrices), colours, `Variant`, reflection-lite properties, signals, `Result`/`Error`, logging, arenas, SHA-256, UUIDs, URLs, locale-aware number formatting, and DEFLATE/zlib compression. |
| **cfw-io** | Files and paths (UTF-8, atomic writes, memory mapping), JSON (strict, fast, with precise error positions), a binary reader/writer, per-user settings and standard folders, processes. |
| **cfw-net** | An event loop, TCP, TLS 1.2/1.3 through the OS, an HTTP/1.1 client, and WebSocket client and server. |
| **cfw-image** | `Image`; PNG and JPEG decoding and encoding, and WebP decoding, exact to the pixel against the reference decoders; resizing and an atlas packer. |
| **cfw-text** | Unicode 18 (grapheme clusters, line breaking, bidi), a TrueType/CFF font parser, OpenType shaping for every script HarfBuzz shapes (Arabic, Hebrew, Thai, Hangul, the Indic scripts, Khmer, Myanmar and the 90-odd scripts of the Universal Shaping Engine), text layout, font fallback and a glyph atlas. |
| **cfw-gfx** | A 2D `Painter`: paths, pens, dashes, gradients, images, transforms, clipping and blend modes, on an anti-aliased CPU rasteriser; SVG icons. |
| **cfw-platform** | Native windows (Win32, X11), keyboard, mouse and input methods (IME), file drag and drop, the clipboard, cursors, high-DPI, and OpenGL 3.3 contexts with a built-in function loader. |
| **cfw-ui** | A retained widget toolkit: layout, focus, themes, buttons, text fields, menus, dropdowns, sliders, tree views, tabs, dockable panels, property grids, dialogs, a colour picker and more. |
| **cfw-app** | Puts it together: UI windows (CPU-painted, or composited over OpenGL views), screen reader support, native file pickers, opening links. |

Modules are layered; each depends only on the ones above it in this table, so a headless server can link
cfw-core, cfw-io and cfw-net and nothing else.

### Platforms

| Platform | Status |
|---|---|
| **Windows** (MinGW-w64 GCC 13) | Supported. |
| **Linux** (X11; GCC 13 or Clang 18) | Supported. |
| **macOS** 13.3+ | Experimental: the AppKit backend is written but has not been built on a Mac yet. |

## Quick start

### Build and run the tests

You need CMake 3.25 or newer, Ninja and a C++20 compiler. On Linux, install the X11 development headers
(`libx11-dev`) for windows; without them CFW builds a headless stub.

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

Or download a prebuilt package (Windows with MinGW-w64, Linux x64) from
[Releases](https://github.com/Clannect/ClannectFramework/releases), unpack it, and use `find_package`:

```cmake
find_package(ClannectFramework 0.1 REQUIRED)   # configure with -DCMAKE_PREFIX_PATH=<unpacked folder>
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
  libjpeg-turbo's and libwebp's pixels. Font parsing
  matches fontTools and FreeType glyph for glyph. Shaping matches HarfBuzz on every glyph id, cluster and
  offset. Text algorithms pass all of Unicode's conformance tests. The references run outside the build to
  produce expected results; they are never linked.
- **Fuzzing.** Every parser (JSON, URLs, HTTP, WebSocket frames, compression, PNG, JPEG, WebP, fonts, SVG,
  shaping, painting) has a libFuzzer target, and the committed corpus replays in every build.
- **Sanitizers.** CI runs the whole suite under ASan, UBSan and LSan, and the networking code under TSan.
- **Real clients.** Input methods, screen reader support and drag and drop are tested against the real system
  components (an X input method, the AT-SPI registry, and Windows' own APIs under Wine).
- **Soak tests.** Long randomised runs of the network stack and the widget toolkit; `CFW_SOAK_SECONDS` makes
  them run longer.

Warnings are errors in CFW's own code.

## Repository layout

```
modules/<module>/include/cfw/<name>/   public headers
modules/<module>/src/                  implementation
modules/<module>/tests/                tests, one executable per area
examples/                              example programs (the widget gallery)
fuzz/                                  fuzz targets and their corpus
bench/                                 benchmarks
testing/                               test helpers and the scripts that produce reference results
docs/decisions/                        design decisions, one file each
```

## Design decisions

Why things are the way they are is recorded in [`docs/decisions`](docs/decisions): for example why
[`String` is `std::string`](docs/decisions/0001-string-is-std-string.md), why CFW
[writes its own codecs](docs/decisions/0012-cfw-is-independent-own-codecs.md) and
[text stack](docs/decisions/0015-cfw-text-own-unicode-fonts-shaping.md), and how
[TLS goes through the operating system](docs/decisions/0013-tls-through-the-os-and-windows-ci.md).

## Contributing

Issues and pull requests are welcome. Please keep to the style of the surrounding code, add tests with
your change, and make sure `ctest --preset debug` passes with no warnings.

## License

Clannect Framework is available under the [Open Use License (OUL) v1.1](license.md). You may use, modify and
distribute it, commercially too, as long as you keep the license and copyright notice, credit Clannect with a
link to this repository, and mark modified versions as modified. See [license.md](license.md) for the full
terms.

Test data from third parties keeps its own license: the DejaVu fonts, PngSuite and the Unicode test files
(see the `testdata` folders).
