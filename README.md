# Clannect Framework (CFW)

A C++20 application framework, written from scratch with no third-party code, that replaced Qt inside Clannect.
The Clannect Engine (the editor, the game client `clannect-player` and the headless Runtime) now runs on it
alone: no Qt is built, linked or shipped. The full brief is
[prompt.md](prompt.md). The project was first named Clannect Engineering (CE); the original brief in the
Clannect Engine repository (`docs/engineering/CLANNECT_ENGINEERING.md`) still uses that name. This README
covers only what is built here and how to work on it.

## Status

| Milestone | State |
|---|---|
| M0 — seams in Clannect | **Done, then superseded:** the engine moved straight onto CFW types instead of keeping Qt behind interfaces. |
| M1 — cfw-core + cfw-io | **Done.** The Runtime builds and passes its tests with no Qt; scene files round-trip byte for byte with the Qt build. |
| M2 — cfw-net + cfw-image | **Done.** The Runtime serves multiplayer sessions over CFW's WebSocket server; the asset cache downloads, decodes and stores through CFW. |
| M3 — cfw-gfx + cfw-text | **Done.** Every shaping engine HarfBuzz has (but AAT) matches it glyph for glyph, including Indic, Khmer and Myanmar. Every interface renders through CFW and was pixel-diffed against Qt; the §7 budgets are met. The GPU paint backend is not written (the CPU backend is fast enough so far). |
| M4 — cfw-platform + cfw-app | **Done on Windows and Linux (X11); written for macOS (AppKit) but not yet compiled there** (see M6). The viewport and `clannect-player` run in CFW windows with CFW input and GL contexts. |
| M5 — cfw-ui + editor port | **Done.** The editor runs on CFW with feature parity, on Windows and Linux; the Qt editor is deleted. |
| M6 — hardening and platforms | **Done except verifying macOS.** Done: Linux builds; Qt gone from every build system and the licence register; fuzzing corpora; the system clipboard on X11; full screen; IME composition (XIM, IMM32); accessibility (AT-SPI on Linux, MSAA on Windows, each tested with a real client); file drag and drop (XDND, OLE); soak tests (network churn, a UI monkey, and the engine's editor soak, which found three memory bugs). **macOS:** the AppKit window, GL, clipboard, IME, drop and file-picker backends are written, with a CI job, but no Mac or macOS SDK was available, so none of it has been compiled ([0016](docs/decisions/0016-macos-backend.md)). macOS accessibility is not written. |

**cfw-core contents:**

- **Errors and contracts:** `Result`/`Error`, `require`/`debugCheck`, `ThreadChecker`.
- **Text:** `String`/`StringView`, UTF-8/UTF-16, string utilities (locale-independent numbers), `Name`.
- **Containers:** `Span`, `SmallVector`, `StableVector`, `FlatMap`, `FlatSet`.
- **Math:** `Vec2`/`Vec2i`/`Vec3`/`Vec4`, `Quat`, `Mat3`, `Mat4`, `Transform2D` (with `quadToQuad`),
  `RectF`/`Recti`, `Color`/`LinearColor`.
- **Values and properties:** `AssetLink`, `Variant`/`VariantArray`, `PropertyInfo`/`ClassInfo`/`PropertyBag`.
- **Signals:** `Signal`/`Connection`/`ScopedConnection`/`SignalOwner`.
- **Identity and encoding:** `Sha256`, `Uuid`, `Url` (with percent- and form-encoding).
- **Runtime support:** `Clock`/`Stopwatch`/`FrameTimer`, `Logger` (structured; no global instance),
  `Arena`/`FrameArena`.

**cfw-io contents:**

- **JSON:** `JsonValue`/`JsonObject`, `parseJson` (limits, precise error positions, iterative), and
  `writeJson`, which is byte-identical to the Qt build's output.
- **Binary:** `ByteWriter`/`ByteReader`, the engine's wire format with sticky-failure reads.
- **Files and directories:** `Path` (UTF-8), `readFile`/`readTextFile` with size limits, `writeFileAtomic`
  (temp + flush + rename), directory operations, `MappedFile`, `TemporaryDirectory`.
- **Per-user locations and settings:** `StandardPaths` (Qt's folder layout), `Settings` (JSON, atomic),
  `FileWatcher` (polling for now).

**cfw-core also has** `Locale` (CLDR number formatting and parsing for ~30 locales, no global locale) and its own DEFLATE/zlib codec (`inflate`/`deflate`, `zlibDecompress`/`zlibCompress`) and
CRC-32/Adler-32. Real zlib decodes everything it writes, at zlib's speed and ratio.

**cfw-image contents** ([0012](docs/decisions/0012-cfw-is-independent-own-codecs.md)):

- **`Image`:** 8-bit RGBA, straight or premultiplied alpha. It never throws, and `ImageLimits` (input bytes,
  dimensions, decoded bytes, aspect ratio) is checked before anything is allocated.
- **PNG:** decodes every colour type, bit depth, palette/tRNS and Adam7 interlacing; all of PngSuite is
  pixel-exact. Encodes RGBA or RGB with adaptive filters.
- **JPEG:** decodes baseline and progressive, any integer subsampling, restart markers, gray, YCbCr, RGB,
  CMYK and YCCK, to exactly libjpeg-turbo's pixels. Encodes baseline JPEG, matching `cjpeg`'s size and quality.
- **WebP:** decodes lossy, lossless, alpha and the first frame of animations, to exactly libwebp's pixels.
- **`decodeImage`** detects the format from the content. **`resizeImage`** offers box, bilinear and Lanczos-3
  filters and works in premultiplied alpha. **`RectPacker`** is a skyline packer for atlases.

**cfw-gfx contents** ([0014](docs/decisions/0014-m3-paths-and-scan-conversion.md)):

- **Geometry:** `PainterPath` (cfw-core; QPainterPath's model) and `Rasterizer` (cfw-image; exact-area
  anti-aliasing, non-zero and even-odd rules). Both are shared with cfw-text.
- **`Painter`:** a save/restore state stack with transforms (perspective included), opacity, blend modes
  (source-over, source, multiply, screen, plus, destination-in/out) and nested rectangle and path clips.
  It fills and strokes paths, and draws images with smooth or nearest sampling, tiling and tint.
- **`Pen`/`Stroker`:** Qt's caps, joins, miter limits and dashes, verified against `QPainterPathStroker`.
- **`Brush`:** solid colours, and linear and radial (focal) gradients with pad, repeat and reflect.
- **`RasterPaintBackend`:** the CPU backend behind the `PaintBackend` seam. It uses Qt's raster arithmetic
  and allocates nothing per frame.

**cfw-text contents** ([0015](docs/decisions/0015-cfw-text-own-unicode-fonts-shaping.md)): Unicode 18
character properties, generated from the UCD into about 110 KB of tables. It has grapheme clusters (UAX #29),
line breaking (UAX #14) and the bidi algorithm (UAX #9, with isolates, brackets and reordering), and passes
all 882,000 cases of the Unicode conformance tests. `FontFace` reads TrueType, CFF (name-keyed and CID) and
collections, and matches fontTools and FreeType glyph for glyph on about 250,000 glyphs. `Shaper` does
OpenType shaping: GSUB and GPOS with every lookup type, and the Arabic, Hebrew and Thai engines. It also covers
fonts without layout tables (marks placed by glyph boxes, Arabic presentation forms, the `kern` table). It
matches HarfBuzz glyph for glyph on 7,930 committed cases over four test fonts, and on every font in the
build container. `TextLayout` wraps, aligns, elides and hit-tests bidi text with font fallback from a
`FontDatabase` of installed fonts. `GlyphCache` rasterises glyphs into an atlas, checked against FreeType, and
`Painter::drawText` draws them. Hangul and the Universal Shaping Engine are done; the Indic, Khmer and Myanmar engines come later.

**cfw-net contents:**

- **Loop and threads:** `EventLoop` (timers, cross-thread `post`, zero CPU when idle) and `Executor` (blocking
  work such as DNS).
- **TCP:** `TcpConnection` and `TcpListener`, with backpressure and write coalescing.
- **TLS:** `TlsStream`, TLS 1.2/1.3 through the operating system (Schannel on Windows, the system OpenSSL
  loaded at runtime on Linux). It verifies against the system trust store or pinned fingerprints; `https://`
  and `wss://` use it ([0013](docs/decisions/0013-tls-through-the-os-and-windows-ci.md)).
- **HTTP:** `HttpClient`, an HTTP/1.1 client with a safe redirect policy, streaming, limits and timeouts.
- **WebSocket:** `WebSocket` (RFC 6455 client and server end, auto-pong, close handshake, limits) and
  `WebSocketServer` (handshake timeout, a bounded number of pending handshakes, 400 on bad requests).
- **Testing:** the frame codec and handshake are pure code, tested against the RFC's own examples and fuzzed.

Against Qt 6.8.3's `QWebSocketServer` on the same workload (200 connections, echoed 48-byte messages), CFW
handles **205,000 messages/s against Qt's ~200,000**, and opens the connections 3.4× faster. It
interoperates with Qt's WebSocket stack in both directions
([0010](docs/decisions/0010-cfw-net-design.md)).

**cfw-platform contents:**

- **`Window`:** native windows on Win32, X11 and AppKit (a stub elsewhere, so headless servers link): title,
  size, full screen, per-monitor DPI, cursors, pointer warping, presenting CPU-painted pixels and capturing
  them back. Pointer (with click counts), key (physical codes), text, resize, DPI, focus, repaint and close
  signals. `processEvents(maxWait)` idles at zero CPU and `wakeUp()` works from any thread.
- **Input methods:** composition events with the caret placed for the candidate window (XIM on-the-spot
  preedit, IMM32, `NSTextInputClient`). Tested on X11 with a real input method (uim with Anthy: "nihonn"
  becomes にほん) and on Windows under Wine.
- **File drops:** `DropEvent`s from XDND 5, OLE `IDropTarget` and `WM_DROPFILES`, and AppKit dragging.
- **Clipboard:** the system clipboard everywhere. On X11 that is the `CLIPBOARD` selection, both ways,
  with incremental (INCR) transfers.
- **OpenGL:** `GlContext` (WGL; GLX with libGL loaded at run time; `NSOpenGLContext`), CFW's own GL 3.3 core
  types and function table (`Gl.h`, no third-party headers or loaders), and `GlBuffer`, `GlVertexArray` and
  `GlShaderProgram`.

**cfw-ui contents:** an element tree hosted by a `Surface` (layout, focus, keyboard and pointer routing,
capture, damage-driven painting, timers, shortcuts, popups, modal dialogs, tooltips and cursors). Controls:
`Label`, `Button`, `TextField` (selection, undo, masking, clipboard), `CheckBox`, `NumberField`, `Dropdown`
(searchable), `Menu` and `MenuBar` with submenus, `ScrollArea`, `Splitter`, `TabBar` (icons, badges),
`TreeView` (virtualised; columns, filtering, rename in place, drag and drop, context menus), `DockLayout` and
`DockPanel` (docking, resizing, saved layouts), `PropertyGrid` (sections, name scrubbing), `Dialog`,
`MessageBox`, `ColorPicker`, `Slider`, `ProgressBar`, and SVG `Icon`s. `examples/ui-gallery` shows them.

**cfw-app contents:** `UiWindow` (a surface in a native window, painted on the CPU, idle when nothing
changes) and `GlUiWindow` (the same with OpenGL views such as a 3D viewport, the interface composited over
them), with close handlers for "save your changes?". Both publish the cfw-ui accessibility tree to screen
readers: MSAA (`IAccessible`) on Windows, and AT-SPI over CFW's own D-Bus client on Linux. Each is tested with
a real client: Wine's oleacc, and pyatspi against the AT-SPI registry. `Desktop.h`: the system's file picker
(the Win32 common dialog; `NSOpenPanel`; zenity or kdialog on Linux), showing a folder in the file manager,
and opening a link in the browser.
`cfw_embed_resources` (CMake) builds files into a binary and finds them by path.

**Benchmarks** ([docs/benchmarks](docs/benchmarks/README.md)): a 5 MB scene parses in **0.62×** and writes in
**0.33×** Qt's time on the same machine. Property access, signals, arena frames, hashing and math run with
zero allocations. That allocation budget is a CTest test in every build, and `--baseline` flags >5%
regressions.

**Scene round-trip:** a real scene saved by the Qt build of the engine, parsed and re-written by CFW, is byte-identical.
So are two Qt-written edge-case documents. `FuzzSmokeTest` runs 45,000 hostile inputs through the parsers on
every build.

**CI** ([0011](docs/decisions/0011-linux-ci-and-fuzzing.md), [0013](docs/decisions/0013-tls-through-the-os-and-windows-ci.md)): on every push, GCC and Clang debug builds, a release build,
a Windows (MinGW-w64) cross build whose tests run under Wine plus a Schannel↔OpenSSL interop check,
ASan+UBSan+LSan over every test, TSan over cfw-net, and 14 libFuzzer targets covering every parser (JSON,
binary reader, URL/UTF-8, WebSocket frames, HTTP, inflate, PNG, JPEG, WebP, painting, text, fonts, shaping
and SVG), fuzzed for 45 s each. The committed corpus (`fuzz/corpus/`) replays as CTest tests on every
toolchain, MinGW included. Window and GL tests run against Xvfb on Linux and skip (passing) without a display.
UBSan is fatal in every sanitizer build. The soak tests (`SoakNetTest`, `SoakUiTest`) run for 2 s in every
build; `CFW_SOAK_SECONDS` runs them longer and `CFW_SOAK_SEED` replays a run. A `macos` workflow (manual only;
macOS is best-effort) builds and tests on GitHub's macOS runners. Fuzzing has found, among others, an out-of-bounds write in the JPEG
Huffman table builder and integer overflow on hostile VP8 coefficients; each fix has a regression input.

**No Qt:** the `NoQt` CTest test fails the build if a Qt header, macro, CMake package or linked Qt library
appears anywhere in CFW's sources, build files or binaries. Where CFW must behave exactly like Qt (Euler angles,
`quadToQuad`), the tests pin numbers captured from Qt once, outside this repository
([0007](docs/decisions/0007-qt-behaviour-pinned-by-oracle-values.md)).

## Building

This needs CMake ≥ 3.25, Ninja and a C++20 compiler: GCC 13 or Clang 18 on Linux, MinGW-w64 GCC 13 on Windows
(a standalone build such as WinLibs; nothing from Qt is needed). On Linux the window system backend needs the
X11 development headers (`libx11-dev`); without them cfw-platform builds its headless stub. On macOS (13.3 or
newer) it needs Xcode's command-line tools; the Cocoa backend is Objective-C++ and has not yet been compiled.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

To build for Windows from Linux: `cmake -S . -B build/windows -G Ninja
-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-cross.cmake`, then run the tests under Wine.

| Preset | What it is |
|---|---|
| `debug` | Contract checks and standard-library bounds checks on. |
| `release` | `-O3`, checks off. |
| `ubsan-trap` | Debug + UBSan without a runtime. This is the only sanitizer MinGW can run. |
| `asan` | Debug + ASan/UBSan/LSan, for Linux or MSVC. See `docs/decisions/0005`. |
| `tsan` | Debug + TSan (Linux), for cfw-net. |
| `fuzz` | Clang + libFuzzer + ASan/UBSan: builds `build/fuzz/fuzz/Fuzz*`. See `docs/decisions/0011`. |

Warnings are errors in CFW's own code.

## Layout

```
cmake/CfwTargets.cmake        cfw_add_module / cfw_add_test and shared compile flags
modules/<module>/include/cfw/<short>/   public headers, one class per header
modules/<module>/src/                  implementation
modules/<module>/tests/                one test executable per behaviour area
testing/include/cfw/test/Check.h        plain-assertion test helpers (engine style)
fuzz/targets/, fuzz/corpus/<Target>/   libFuzzer targets and their committed corpus
docs/decisions/NNNN-title.md           decision log
```

Lower modules never depend on higher ones (spec §3). cfw-core has no third-party dependencies and makes no OS
calls.

## Third-party dependencies

**None, by design** ([0012](docs/decisions/0012-cfw-is-independent-own-codecs.md)). Clannect Framework is a
framework in its own right: compression, image codecs, Unicode, fonts, shaping, rasterisation, networking,
TLS plumbing, the platform layer and the widgets are all written here. CFW links only the C++ standard library
and the operating system:

- **Windows:** `user32`, `gdi32`, `opengl32`, `shell32`, `ole32`, `comdlg32`, `ws2_32`, and `secur32`/`crypt32`/`ncrypt`
  (Schannel TLS).
- **Linux:** POSIX, Xlib, and at run time `libGL` and the system OpenSSL (both loaded with `dlopen`), plus
  `zenity`/`kdialog` and `xdg-open` for the file picker and links when they are installed, and the session
  D-Bus for accessibility.
- **macOS:** AppKit, Carbon (key codes only), UniformTypeIdentifiers, the OpenGL framework (loaded with
  `dlopen`), and Homebrew's or MacPorts' OpenSSL 3 at run time when one is installed.

Reference implementations (zlib, libjpeg-turbo, libwebp, Pillow, and Qt 6.11 for 2D rendering via
`testing/qt-oracle`) are run outside the build to produce the expected outputs that tests compare against. They are never built or linked. The only third-party
*data* in the repository is PngSuite (`modules/cfw-image/testdata/pngsuite`, free for any use), used as test
input.

## Decisions so far

- [0001](docs/decisions/0001-string-is-std-string.md) — `cfw::String` is `std::string`.
- [0002](docs/decisions/0002-name-is-hashed-not-interned.md) — `Name` is hashed, not interned (constraint 5
  forbids a global table).
- [0003](docs/decisions/0003-bounds-checks-via-stdlib-hardening.md) — debug bounds checks come from
  standard-library hardening.
- [0004](docs/decisions/0004-signal-slot-lifetime.md) — why signals use shared ownership.
- [0005](docs/decisions/0005-sanitizers-and-the-windows-toolchain.md) — MinGW has no sanitizer runtimes, so
  CI needs Linux from M1 (**amends §8**).
- [0006](docs/decisions/0006-plain-assertion-tests.md) — plain-assertion tests rather than Catch2/doctest.
- [0007](docs/decisions/0007-qt-behaviour-pinned-by-oracle-values.md) — Euler/projection behaviour pinned by
  numbers captured from Qt; the deliberate differences are listed there.
- [0008](docs/decisions/0008-data-driven-property-schema.md) — properties are a runtime schema plus indexed
  storage; native accessors are deferred to M5.
- [0010](docs/decisions/0010-cfw-net-design.md) — cfw-net: started before M1 closed, a poll reactor, write
  coalescing (a 6× speed-up), no TLS yet.
- [0009](docs/decisions/0009-cfw-io-compatibility-and-scope.md) — Qt-compatible JSON, `.cescene` must be
  `-text` in git, registry-to-JSON settings migration, polling file watcher.
- [0011](docs/decisions/0011-linux-ci-and-fuzzing.md) — Linux CI (GCC, Clang, ASan/LSan, TSan), libFuzzer
  targets with a committed corpus, and the three bugs they found on the first run.
- [0012](docs/decisions/0012-cfw-is-independent-own-codecs.md) — CFW ships no third-party code (**amends §2.1,
  §4.4, §11**): own DEFLATE and PNG/JPEG/WebP codecs, reference-exact tests, fuzzing, performance.
- [0013](docs/decisions/0013-tls-through-the-os-and-windows-ci.md) — TLS through the OS (Schannel; system
  OpenSSL via `dlopen`), `https://`/`wss://`, a Windows cross build tested under Wine, and a use-after-free it
  caught.
- [0015](docs/decisions/0015-cfw-text-own-unicode-fonts-shaping.md) — cfw-text writes its own Unicode
  algorithms, font parsing and shaping (**amends §4.5, §11**: HarfBuzz, FreeType and ICU become test
  references); the Unicode layer and its conformance results.
- [0014](docs/decisions/0014-m3-paths-and-scan-conversion.md) — M3: `PainterPath` (renamed from the spec's
  `Path`, which is cfw-io's file path), the rasteriser and the CPU painter. It records Qt behaviour measured
  with an outside oracle and CFW's deliberate differences, along with golden images, budgets and fuzzing.
- [0016](docs/decisions/0016-macos-backend.md) — the macOS backend: AppKit from Objective-C++,
  `cfw::fromChars` for Apple's libc++, and why it is unverified until the macOS CI job runs.
