# 0005 — Sanitizers need a second toolchain; MinGW gets trap-mode UBSan only

**Status:** accepted, 2026-09-27. This **amends the reliability requirements**.

## Problem

The reliability requirements ask for ASan+UBSan on every commit, TSan for `cfw-net`/`cfw-app`, and a leak check that fails the build.
Clannect's reference toolchain is MinGW-w64 GCC 13.1, which ships **no sanitizer runtimes**: no ASan, no
TSan, no LSan, and no UBSan runtime. These requirements cannot run on it.

## Decision

- **MinGW (the reference toolchain):** the `ubsan-trap` preset builds with
  `-fsanitize=undefined -fsanitize-undefined-trap-on-error`. That needs no runtime and turns any detected UB
  into a crash. It runs locally and in CI.
- **ASan + UBSan + LSan (leak check):** the `asan` preset on Linux (GCC or Clang). cfw-core and cfw-io have no
  OS-specific code, so Linux runs cover them fully. LSan is what fails the build on a single leak.
- **TSan:** Linux, for `cfw-net` and `cfw-app`, once they exist.
- **Windows-specific code** (`cfw-platform`, the IOCP event loop): MSVC or clang-cl with `/fsanitize=address`.
  This covers ASan only; Windows has no TSan or LSan.

So the CI matrix needs at least **Linux GCC/Clang** from M1 onwards, not from M6 as the milestone plan implied. That is
cheap: cfw-core already builds with no platform code.

## What would change this

A MinGW distribution with sanitizer runtimes (for example LLVM-MinGW with compiler-rt's ASan, which works for
some configurations). We would evaluate it then.
