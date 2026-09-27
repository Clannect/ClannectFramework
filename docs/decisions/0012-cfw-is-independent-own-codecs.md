# 0012 — CFW is independent: no third-party code, own image codecs and compression

**Status:** accepted, 2026-09-27. Direction from the project owner. This **amends §2.1, §4.4 and §11** of the
spec, which proposed permissive third-party libraries (Blend2D, FreeType/HarfBuzz, SDL3, libspng,
libjpeg-turbo, libwebp, zlib, BoringSSL/mbedTLS).

## Decision

Clannect Framework is a framework in its own right. It ships **no third-party code**: every format, codec
and algorithm it needs is written in this repository, under its tests, fuzzing and review. The spec's §11 list
now names *references* to test against, not dependencies. The only things CFW links are the C++ standard
library and the operating system's own APIs (sockets, files, windows, GL/graphics drivers).

Consequences for the rest of the plan:

- **cfw-gfx** gets its own CPU rasteriser; Blend2D is out.
- **cfw-text** gets its own TrueType/OpenType parsing, rasterisation and shaping; FreeType and HarfBuzz are
  out. Shaping complex scripts is the largest single item this adds; §4.5 already said to budget for it.
- **cfw-platform** is written against Win32/X11/Wayland/Cocoa directly; SDL and GLFW are out.
- **TLS** uses the OS stack (Schannel on Windows, which `TlsSchannel.cpp` has started; the platform's own
  TLS elsewhere) rather than BoringSSL or mbedTLS.
- The "register" of dependencies in the Clannect Engine README records CFW as having none.

## Why this is safe

§4.4 preferred hardened libraries over hand-rolled decoders because hand-rolled parsers are where memory
bugs live. The answer here is the same hardening, applied to our own code:

- **Reference-exact tests.** Every decoder is checked pixel for pixel against the reference implementation
  of its format, on corpora that cover each feature. The references were run once, outside the repository;
  only their outputs (as SHA-256 hashes) and the inputs are committed:
  - PNG: all 161 valid PngSuite files (Willem van Schaik's conformance suite, free for any use) decode
    pixel-exact, and its 14 corrupt files are rejected.
  - JPEG: 78 files from libjpeg-turbo 2.1.5's `cjpeg` decode to exactly what its `djpeg` produces. They
    cover baseline, progressive with successive approximation, ten chroma sampling layouts, restart markers,
    grayscale, RGB and CMYK.
  - WebP: 74 files from libwebp 1.3.2's `cwebp`/`img2webp` decode to exactly what `dwebp` produces. They
    cover lossy at every filter, sharpness and segment setting, lossless at every effort level and palette
    size, alpha with every method and filter, and animation.
  - DEFLATE: decodes streams real zlib wrote, and real zlib decodes everything CFW writes.
- **Fuzzing** (libFuzzer + ASan + UBSan, see 0011) of every decoder, with the corpus committed.
- **Limits before allocation** (`ImageLimits`, `DecompressLimits`), and every failure is a `Result`.
- **Sanitizers are fatal**: any UBSan report fails the test or fuzz run.

The first fuzzing runs found two real bugs, both now fixed with regression inputs. One was an out-of-bounds
write in the JPEG Huffman table builder in release builds. The other was integer overflow on hostile VP8
coefficients. This is why the fuzzing is not optional.

## Where the tables come from

Some formats define large constant tables: VP8's coefficient and mode probabilities (RFC 6386), and the
JPEG Annex K tables. These are part of each format's definition. They were taken from the specifications'
published tables (for VP8, the reference implementation's BSD-licensed sources, which reproduce RFC 6386)
by a script, as data, and checked by the reference-exact tests above. No code was taken.

## Performance (3 megapixels, one core, release build)

| | CFW | Reference |
|---|---|---|
| inflate | 212 MB/s | zlib 195 MB/s |
| deflate (level 6) | ratio within 1% of zlib | |
| JPEG baseline decode | 39 ms | libjpeg-turbo 20 ms (SIMD), ~27 ms (scalar C) |
| JPEG progressive decode | 70 ms | libjpeg-turbo 46 ms (SIMD), ~52 ms (scalar C) |
| WebP lossless decode | 95 ms | libwebp 78 ms |
| WebP lossy decode | 102 ms | libwebp 44 ms (SIMD) |

Decoding runs on the asset loader's worker threads, not in a frame. The gap to libjpeg-turbo and libwebp
comes from their SIMD. Closing it is future work (the colour conversion, IDCT and loop filters vectorise
well). §7 has no decode budget, so it is not a blocker; it is recorded here so it is not forgotten.

## What would change this

Nothing short of a format CFW cannot reasonably implement itself. If that happens, the answer is a new
decision record, not a quiet dependency.
