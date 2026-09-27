# 0006 — Tests use plain assertions, not Catch2 or doctest (for now)

**Status:** accepted, 2026-09-27

## Decision

§11 proposes Catch2 or doctest. §9 and §12 ask for tests "in the existing style: small, named after the
behaviour, no framework magic". The engine's tests are plain executables with a `check()` function and a
failure count, so CFW uses the same style through `testing/include/cfw/test/Check.h`:
`check`, `checkEqual`, `checkNear` and `finish`. There is one executable per behaviour area, registered with
CTest and given a 10-second timeout.

This adds no dependency, compiles fast, and any engine developer can read it without learning a framework.

## What would change this

- Needing parameterised or property-based tests at scale (§9 asks for property-based tests of layout and text
  metrics). If hand-rolled generators become awkward, adopt doctest (MIT) then and record it in the register.
- Needing per-test filtering in CI beyond what one-binary-per-area gives.
