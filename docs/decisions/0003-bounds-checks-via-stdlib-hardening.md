# 0003 — Debug bounds checks come from standard-library hardening

**Status:** accepted, 2026-09-27

## Decision

§8 requires debug builds to bounds-check "every container and every `Span`". CFW uses the standard library's
own checked modes rather than wrapping `std::vector` or `std::span`:

- GCC/Clang with libstdc++: Debug builds define `_GLIBCXX_ASSERTIONS`, which checks `operator[]`, `front()`,
  `back()`, `std::span` indexing, and more. libc++ has the equivalent `_LIBCPP_HARDENING_MODE`, to be added
  when a libc++ toolchain joins CI.
- MSVC: Debug builds already use `_ITERATOR_DEBUG_LEVEL=2`.
- CFW's own containers (`SmallVector`, `VariantArray`) check with `cfw::debugCheck`, which is on whenever
  `CFW_DEBUG_CHECKS` is defined (Debug builds).

`cfw::Span` is therefore a plain alias for `std::span`.

The definitions are PUBLIC on each module target, so dependants compile headers with the same checks as the
library. Mixing hardened and unhardened translation units is an ODR hazard for these flags.

## What would change this

A platform whose standard library has no checked mode. We would then wrap `Span` there.
